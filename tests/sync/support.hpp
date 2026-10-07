#pragma once

#include "onedrive/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "sync/filter/selective.hpp"
#include "support/sync.hpp"
#include "support/common.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::test::sync {

using onedrive::test::TemporaryDirectory;

class FakeGraphClient final {
public:
    [[nodiscard]] onedrive::graph::NotificationChannel
    notification_channel() const {
        return {
            .notification_url = "https://notification.example.test/token",
            .expires_at =
                std::chrono::system_clock::now() + std::chrono::hours{1},
        };
    }

    void refresh_access_token() const {
    }

    [[nodiscard]] onedrive::account::DriveIdentity drive_identity() const {
        return onedrive::test::test_drive_identity();
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    item_by_path(const std::string& path) const {
        if (lookup_item && lookup_item->remote_path == path) {
            return *lookup_item;
        }
        throw std::logic_error{"single path lookup was not expected"};
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const onedrive::graph::DeltaProgress& progress
    ) const {
        delta_requests.push_back(delta_link);
        if (delta_link && reject_saved_cursor) {
            throw onedrive::graph::DeltaCursorInvalidError{
                "simulated invalid delta cursor"
            };
        }
        if (progress) {
            progress(
                1, changes.size(), onedrive::util::ProgressState::completed
            );
        }
        return {
            .changes = changes,
            .delta_link = "https://graph.example.test/delta",
        };
    }

    void download_file(
        const std::string& remote_id,
        const std::string&,
        std::uint64_t,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        std::stop_token stop_token,
        const onedrive::graph::DownloadProgress& progress,
        const onedrive::graph::DownloadCheckpoint& checkpoint,
        const onedrive::graph::DownloadData& data
    ) const {
        ++download_count;
        last_download_offset.store(initial_offset, std::memory_order_relaxed);
        const int active =
            active_downloads.fetch_add(1, std::memory_order_relaxed) + 1;
        int maximum =
            maximum_concurrent_downloads.load(std::memory_order_relaxed);
        while (active > maximum &&
               !maximum_concurrent_downloads.compare_exchange_weak(
                   maximum, active, std::memory_order_relaxed
               )) {
        }
        struct ActiveDownloadGuard {
            std::atomic_int& count;
            ~ActiveDownloadGuard() {
                count.fetch_sub(1, std::memory_order_relaxed);
            }
        } guard{active_downloads};
        if (download_delay > std::chrono::milliseconds::zero()) {
            const auto deadline =
                std::chrono::steady_clock::now() + download_delay;
            while (std::chrono::steady_clock::now() < deadline) {
                if (stop_token.stop_requested()) {
                    throw onedrive::graph::DownloadCancelledError{
                        "simulated download cancellation"
                    };
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }

        if (remote_id == failing_id) {
            while (download_count.load(std::memory_order_relaxed) <
                       downloads_started_before_failure ||
                   checkpoint_count.load(std::memory_order_relaxed) <
                       checkpoints_before_failure) {
                std::this_thread::yield();
            }
            throw std::runtime_error{"simulated download failure"};
        }
        if (remote_id == cancellable_id) {
            const auto durable_bytes = std::min<std::size_t>(
                cancellation_checkpoint, contents.at(remote_id).size()
            );
            {
                std::ofstream output{destination, std::ios::binary};
                output << contents.at(remote_id).substr(0, durable_bytes);
            }
            if (checkpoint) {
                checkpoint(durable_bytes);
            }
            checkpoint_count.fetch_add(1, std::memory_order_relaxed);
            while (!stop_token.stop_requested()) {
                std::this_thread::yield();
            }
            throw onedrive::graph::DownloadCancelledError{
                "simulated download cancellation"
            };
        }
        if (before_download_write) {
            before_download_write(remote_id);
        }
        if (initial_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << contents.at(remote_id);
        } else {
            std::fstream output{
                destination, std::ios::in | std::ios::out | std::ios::binary
            };
            output.seekp(static_cast<std::streamoff>(initial_offset));
            output << contents.at(remote_id).substr(initial_offset);
        }
        if (data) {
            const std::string_view downloaded{contents.at(remote_id)};
            data(
                initial_offset,
                std::as_bytes(
                    std::span{downloaded.substr(
                        static_cast<std::size_t>(initial_offset)
                    )}
                )
            );
        }
        if (progress) {
            const auto size = contents.at(remote_id).size();
            progress(initial_offset + (size - initial_offset) / 2, size);
            progress(size, size);
        }
        if (checkpoint) {
            checkpoint(contents.at(remote_id).size());
        }
    }

    [[nodiscard]] onedrive::graph::RemoteItem upload_file(
        const std::string& remote_path,
        const std::optional<std::string>& remote_id,
        const std::string&,
        const std::filesystem::path& source,
        const std::optional<onedrive::graph::UploadSession>& session,
        const onedrive::graph::UploadCheckpoint& checkpoint,
        std::stop_token
    ) const {
        int upload_number = 0;
        {
            const std::scoped_lock lock{upload_mutex};
            upload_number = ++upload_count;
            uploaded_paths.push_back(remote_path);
            upload_sessions.push_back(session);
        }
        const int active =
            active_uploads.fetch_add(1, std::memory_order_relaxed) + 1;
        int maximum =
            maximum_concurrent_uploads.load(std::memory_order_relaxed);
        while (active > maximum &&
               !maximum_concurrent_uploads.compare_exchange_weak(
                   maximum, active, std::memory_order_relaxed
               )) {
        }
        struct ActiveUploadGuard {
            std::atomic_int& count;
            ~ActiveUploadGuard() {
                count.fetch_sub(1, std::memory_order_relaxed);
            }
        } guard{active_uploads};
        if (upload_delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(upload_delay);
        }
        if (fatal_upload_error) {
            throw std::runtime_error{"simulated fatal upload failure"};
        }
        if (upload_checkpoint) {
            const auto state = *upload_checkpoint;
            upload_checkpoint.reset();
            checkpoint(state);
            if (fail_after_upload_checkpoint) {
                throw std::runtime_error{
                    "simulated interrupted upload session"
                };
            }
        }
        if (upload_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated completed upload"
            };
        }
        if (remote_path == upload_resource_error_path) {
            throw onedrive::graph::UploadResourceError{
                "remote_quota", "simulated OneDrive quota exhaustion"
            };
        }
        if (before_upload_return) {
            std::function<void()> callback;
            {
                const std::scoped_lock lock{upload_mutex};
                callback = std::move(before_upload_return);
                before_upload_return = {};
            }
            callback();
        }
        return {
            .id =
                remote_id.value_or("uploaded-" + std::to_string(upload_number)),
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "uploaded-etag-" + std::to_string(upload_number),
            .parent_id = "root-id",
            .remote_path = remote_path,
            .last_modified = "2026-10-04T09:00:00Z",
            .size =
                static_cast<std::int64_t>(std::filesystem::file_size(source)),
        };
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    create_directory(const std::string& remote_path) const {
        ++directory_create_count;
        created_directory_paths.push_back(remote_path);
        remote_mutations.push_back("mkdir:" + remote_path);
        if (directory_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated directory conflict"
            };
        }
        if (remote_path == directory_resource_error_path) {
            throw onedrive::graph::UploadResourceError{
                "remote_quota", "simulated OneDrive quota exhaustion"
            };
        }
        if (before_directory_return) {
            auto callback = std::move(before_directory_return);
            before_directory_return = {};
            callback();
        }
        return {
            .id = "directory-" + std::to_string(directory_create_count),
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "directory-etag-" + std::to_string(directory_create_count),
            .parent_id = "root-id",
            .remote_path = remote_path,
            .directory = true,
        };
    }

    void delete_item(
        const std::string& remote_id, const std::string& expected_etag
    ) const {
        deleted_items.emplace_back(remote_id, expected_etag);
        if (delete_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated deletion conflict"
            };
        }
    }

    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string& remote_id,
        const std::string&,
        const std::string& destination_path
    ) const {
        moved_remote_items.emplace_back(remote_id, destination_path);
        remote_mutations.push_back(
            "move:" + remote_id + ":" + destination_path
        );
        if (move_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated remote move conflict"
            };
        }
        return {
            .id = remote_id,
            .name = std::filesystem::path{destination_path}.filename().string(),
            .etag = "moved-etag",
            .parent_id = "moved-parent",
            .remote_path = destination_path,
            .last_modified = "2026-10-05T02:00:00Z",
            .directory = moved_item_directory,
        };
    }

    std::vector<onedrive::graph::RemoteItem> changes;
    std::optional<onedrive::graph::RemoteItem> lookup_item;
    std::unordered_map<std::string, std::string> contents;
    std::string failing_id;
    std::string cancellable_id;
    std::function<void(const std::string&)> before_download_write;
    mutable std::function<void()> before_upload_return;
    mutable std::function<void()> before_directory_return;
    mutable std::optional<onedrive::graph::UploadSession> upload_checkpoint;
    bool reject_saved_cursor{false};
    bool upload_conflict{false};
    bool fatal_upload_error{false};
    std::string upload_resource_error_path;
    bool directory_conflict{false};
    std::string directory_resource_error_path;
    bool delete_conflict{false};
    bool move_conflict{false};
    bool moved_item_directory{false};
    bool fail_after_upload_checkpoint{false};
    std::chrono::milliseconds download_delay{0};
    std::chrono::milliseconds upload_delay{0};
    int downloads_started_before_failure{0};
    int checkpoints_before_failure{0};
    std::size_t cancellation_checkpoint{0};
    mutable std::atomic_int download_count{0};
    mutable int upload_count{0};
    mutable std::mutex upload_mutex;
    mutable int directory_create_count{0};
    mutable std::vector<std::string> uploaded_paths;
    mutable std::vector<std::string> created_directory_paths;
    mutable std::vector<std::pair<std::string, std::string>> deleted_items;
    mutable std::vector<std::pair<std::string, std::string>> moved_remote_items;
    mutable std::vector<std::string> remote_mutations;
    mutable std::vector<std::optional<onedrive::graph::UploadSession>>
        upload_sessions;
    mutable std::atomic_int checkpoint_count{0};
    mutable std::atomic_int active_downloads{0};
    mutable std::atomic_int maximum_concurrent_downloads{0};
    mutable std::atomic_int active_uploads{0};
    mutable std::atomic_int maximum_concurrent_uploads{0};
    mutable std::atomic_uint64_t last_download_offset{0};
    mutable std::vector<std::optional<std::string>> delta_requests;
};

class FakeItemStore final {
public:
    void open() {
    }

    void open_read_only() {
    }

    void upsert(onedrive::storage::ItemState item) {
        const std::scoped_lock lock{mutex};
        if (fail_upsert) {
            throw std::runtime_error{"simulated item persistence failure"};
        }
        ++upsert_count;
        items.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta delta) {
        const std::scoped_lock lock{mutex};
        if (fail_apply_delta) {
            throw std::runtime_error{"simulated delta persistence failure"};
        }
        ++apply_count;
        if (delta.apply_mode == onedrive::storage::DeltaApplyMode::replace) {
            items.clear();
            blocked.clear();
        }
        for (const auto& remote_id : delta.removals) {
            items.erase(remote_id);
        }
        for (const auto& remote_id : delta.partial_download_removals) {
            partials.erase(remote_id);
        }
        for (const auto& item : delta.upserts) {
            pending_moves_by_id.erase(item.remote_id);
            items.insert_or_assign(item.remote_id, item);
        }
        for (const auto& remote_id : delta.removals) {
            pending_moves_by_id.erase(remote_id);
        }
        for (const auto& suppression : delta.upload_suppressions) {
            upload_suppressions_by_path.insert_or_assign(
                suppression.local_path.lexically_normal().string(), suppression
            );
        }
        for (const auto& remote_id : delta.blocked_removals) {
            std::erase_if(blocked, [&remote_id](const auto& item) {
                return item.remote_id == remote_id;
            });
        }
        for (const auto& item : delta.blocked_upserts) {
            const auto existing = std::ranges::find(
                blocked,
                item.remote_id,
                &onedrive::storage::BlockedItem::remote_id
            );
            if (existing == blocked.end()) {
                blocked.push_back(item);
            } else {
                *existing = item;
            }
        }
        applied_delta = std::move(delta);
    }

    void save_pending_download(onedrive::storage::PendingDownload download) {
        const std::scoped_lock lock{mutex};
        pending.insert_or_assign(download.item.remote_id, std::move(download));
    }

    void
    remove_pending_download(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        pending.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingDownload> result;
        for (const auto& [remote_id, download] : pending) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_partial_download(onedrive::storage::PartialDownload download) {
        const std::scoped_lock lock{mutex};
        partials.insert_or_assign(download.item.remote_id, std::move(download));
    }

    void
    remove_partial_download(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        partials.erase(remote_id);
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(const std::string&, const std::string& remote_id) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = partials.find(remote_id);
        return iterator == partials.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::PartialDownload>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::vector<onedrive::storage::PartialDownload>
    partial_downloads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PartialDownload> result;
        result.reserve(partials.size());
        for (const auto& [remote_id, download] : partials) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_pending_upload(onedrive::storage::PendingUpload upload) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_upload_save) {
            throw std::runtime_error{
                "simulated pending upload persistence failure"
            };
        }
        if (fail_upload_checkpoint_save && !upload.upload_url.empty()) {
            throw std::runtime_error{
                "simulated upload checkpoint persistence failure"
            };
        }
        pending_uploads_by_path.insert_or_assign(
            upload.remote_path, std::move(upload)
        );
    }

    void
    remove_pending_upload(const std::string&, const std::string& remote_path) {
        const std::scoped_lock lock{mutex};
        pending_uploads_by_path.erase(remote_path);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingUpload> result;
        for (const auto& [path, upload] : pending_uploads_by_path) {
            static_cast<void>(path);
            result.push_back(upload);
        }
        return result;
    }

    void commit_upload(
        const onedrive::storage::PendingUpload& upload,
        onedrive::storage::ItemState item
    ) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_upload) {
            throw std::runtime_error{"simulated upload commit failure"};
        }
        ++upsert_count;
        items.insert_or_assign(item.remote_id, std::move(item));
        pending_uploads_by_path.erase(upload.remote_path);
    }

    void save_pending_delete(onedrive::storage::PendingDelete deletion) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_delete_save) {
            throw std::runtime_error{
                "simulated pending deletion persistence failure"
            };
        }
        pending_deletes_by_id.insert_or_assign(
            deletion.remote_id, std::move(deletion)
        );
    }

    void
    remove_pending_delete(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        pending_deletes_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDelete>
    pending_deletes(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingDelete> result;
        for (const auto& [remote_id, deletion] : pending_deletes_by_id) {
            static_cast<void>(remote_id);
            result.push_back(deletion);
        }
        return result;
    }

    void commit_delete(const onedrive::storage::PendingDelete& deletion) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_delete) {
            throw std::runtime_error{"simulated deletion commit failure"};
        }
        for (auto iterator = items.begin(); iterator != items.end();) {
            const auto& path = iterator->second.remote_path;
            const bool descendant = path.size() > deletion.remote_path.size() &&
                                    path.starts_with(deletion.remote_path) &&
                                    path[deletion.remote_path.size()] == '/';
            if (iterator->first == deletion.remote_id ||
                path == deletion.remote_path || descendant) {
                iterator = items.erase(iterator);
            } else {
                ++iterator;
            }
        }
        pending_deletes_by_id.erase(deletion.remote_id);
    }

    void save_pending_remote_move(onedrive::storage::PendingRemoteMove move) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_remote_move_save) {
            throw std::runtime_error{
                "simulated pending remote move persistence failure"
            };
        }
        pending_remote_moves_by_id.insert_or_assign(
            move.remote_id, std::move(move)
        );
    }

    void remove_pending_remote_move(
        const std::string&, const std::string& remote_id
    ) {
        const std::scoped_lock lock{mutex};
        pending_remote_moves_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingRemoteMove> result;
        for (const auto& [remote_id, move] : pending_remote_moves_by_id) {
            static_cast<void>(remote_id);
            result.push_back(move);
        }
        return result;
    }

    void commit_remote_move(
        const onedrive::storage::PendingRemoteMove& move,
        onedrive::storage::ItemState item
    ) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_remote_move) {
            throw std::runtime_error{"simulated remote move commit failure"};
        }
        if (move.directory) {
            for (auto& [remote_id, state] : items) {
                if (remote_id == move.remote_id) {
                    continue;
                }
                const auto relative_remote =
                    std::filesystem::path{state.remote_path}.lexically_relative(
                        move.source_remote_path
                    );
                const auto relative_local =
                    state.local_path.lexically_relative(move.source_local_path);
                if (!relative_remote.empty() &&
                    !relative_remote.native().starts_with("..") &&
                    relative_remote == relative_local) {
                    state.remote_path =
                        (std::filesystem::path{move.destination_remote_path} /
                         relative_remote)
                            .generic_string();
                    state.local_path =
                        move.destination_local_path / relative_local;
                }
            }
        }
        items.insert_or_assign(item.remote_id, std::move(item));
        pending_remote_moves_by_id.erase(move.remote_id);
    }

    void save_pending_move(onedrive::storage::PendingMove move) {
        const std::scoped_lock lock{mutex};
        pending_moves_by_id.insert_or_assign(move.remote_id, std::move(move));
    }

    void remove_pending_move(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        pending_moves_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingMove> result;
        for (const auto& [remote_id, move] : pending_moves_by_id) {
            static_cast<void>(remote_id);
            result.push_back(move);
        }
        return result;
    }

    [[nodiscard]] std::vector<onedrive::storage::UploadSuppression>
    upload_suppressions(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::UploadSuppression> result;
        for (const auto& [path, suppression] : upload_suppressions_by_path) {
            static_cast<void>(path);
            result.push_back(suppression);
        }
        return result;
    }

    void remove_upload_suppression(
        const std::string&, const std::filesystem::path& local_path
    ) {
        const std::scoped_lock lock{mutex};
        upload_suppressions_by_path.erase(local_path.lexically_normal().string()
        );
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem>
    blocked_items(const std::string&) const {
        const std::scoped_lock lock{mutex};
        return blocked;
    }

    bool reset(const std::string&) {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) {
        return {};
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string&) const {
        return saved_delta_link;
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string&) const {
        return saved_sync_filter_fingerprint;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState>
    find(const std::string&, const std::string& remote_id) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = items.find(remote_id);
        return iterator == items.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::ItemState>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock{mutex};
        return items.size();
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState>
    drive_items(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::ItemState> result;
        result.reserve(items.size());
        for (const auto& [remote_id, item] : items) {
            static_cast<void>(remote_id);
            result.push_back(item);
        }
        return result;
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> items;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::unordered_map<std::string, onedrive::storage::PartialDownload>
        partials;
    std::unordered_map<std::string, onedrive::storage::PendingUpload>
        pending_uploads_by_path;
    std::unordered_map<std::string, onedrive::storage::PendingDelete>
        pending_deletes_by_id;
    std::unordered_map<std::string, onedrive::storage::PendingRemoteMove>
        pending_remote_moves_by_id;
    std::unordered_map<std::string, onedrive::storage::PendingMove>
        pending_moves_by_id;
    std::unordered_map<std::string, onedrive::storage::UploadSuppression>
        upload_suppressions_by_path;
    std::vector<onedrive::storage::BlockedItem> blocked;
    onedrive::storage::ItemDelta applied_delta;
    std::optional<std::string> saved_delta_link;
    std::optional<std::string> saved_sync_filter_fingerprint;
    int upsert_count{0};
    int apply_count{0};
    bool fail_upsert{false};
    bool fail_commit_upload{false};
    bool fail_pending_upload_save{false};
    bool fail_commit_delete{false};
    bool fail_pending_delete_save{false};
    bool fail_commit_remote_move{false};
    bool fail_pending_remote_move_save{false};
    bool fail_upload_checkpoint_save{false};
    bool fail_apply_delta{false};
    mutable std::mutex mutex;
};

class FakeMetrics final {
public:
    void record_sync_run(
        onedrive::metrics::SyncRunOutcome outcome, std::chrono::duration<double>
    ) noexcept {
        last_success = outcome == onedrive::metrics::SyncRunOutcome::succeeded;
    }

    bool last_success{false};
};

using onedrive::test::fail;

inline onedrive::config::Config
config_for(const std::filesystem::path& root, bool dry_run) {
    auto config = onedrive::config::Config::defaults();
    config.sync_data_directory = root;
    config.state_directory = root.parent_path() / "state";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
    config.sync_mode = onedrive::sync::SyncMode::download_only;
    config.dry_run = dry_run;
    return config;
}

inline onedrive::graph::RemoteItem
file(std::string id, std::string path, std::int64_t size) {
    return {
        .id = std::move(id),
        .name = std::filesystem::path{path}.filename().string(),
        .etag = "etag",
        .parent_id = "parent",
        .remote_path = std::move(path),
        .last_modified = "2026-10-02T00:00:00Z",
        .size = size,
        .directory = false,
        .deleted = false,
    };
}

inline onedrive::storage::ItemState tracked_item(
    const std::filesystem::path& root,
    std::string id,
    std::string path,
    bool directory = false
) {
    const auto local_path = root / path;
    return {
        .drive_id = "me",
        .remote_id = std::move(id),
        .name = local_path.filename().string(),
        .etag = "etag",
        .remote_path = std::move(path),
        .local_path = local_path,
        .last_modified = "2026-10-02T00:00:00Z",
        .size = directory ? 0 : 4,
        .local_size = directory ? 0 : 4,
        .local_modified_ticks =
            directory ? 0
                      : std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::filesystem::last_write_time(local_path)
                                .time_since_epoch()
                        )
                            .count(),
        .directory = directory,
    };
}

inline onedrive::graph::RemoteItem deleted_item(std::string id) {
    return {
        .id = std::move(id),
        .deleted = true,
    };
}

} // namespace onedrive::test::sync
