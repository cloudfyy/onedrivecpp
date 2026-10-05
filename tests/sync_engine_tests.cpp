#include "onedrive/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "sync/filter/selective.hpp"
#include "sync_test_support.hpp"
#include "test_support.hpp"

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

namespace {

using onedrive::test::TemporaryDirectory;

class FakeGraphClient final {
public:
    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const {
        return onedrive::test::test_drive_identity();
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& path
    ) const {
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
            progress(1, changes.size(), true);
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
        last_download_offset.store(
            initial_offset,
            std::memory_order_relaxed
        );
        const int active =
            active_downloads.fetch_add(1, std::memory_order_relaxed) + 1;
        int maximum =
            maximum_concurrent_downloads.load(std::memory_order_relaxed);
        while (active > maximum &&
               !maximum_concurrent_downloads.compare_exchange_weak(
                   maximum,
                   active,
                   std::memory_order_relaxed
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
                cancellation_checkpoint,
                contents.at(remote_id).size()
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
                destination,
                std::ios::in | std::ios::out | std::ios::binary
            };
            output.seekp(static_cast<std::streamoff>(initial_offset));
            output << contents.at(remote_id).substr(initial_offset);
        }
        if (data) {
            const std::string_view downloaded{contents.at(remote_id)};
            data(
                initial_offset,
                std::as_bytes(std::span{
                    downloaded.substr(
                        static_cast<std::size_t>(initial_offset)
                    )
                })
            );
        }
        if (progress) {
            const auto size = contents.at(remote_id).size();
            progress(
                initial_offset + (size - initial_offset) / 2,
                size
            );
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
        const onedrive::graph::UploadCheckpoint& checkpoint
    ) const {
        ++upload_count;
        uploaded_paths.push_back(remote_path);
        upload_sessions.push_back(session);
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
                "remote_quota",
                "simulated OneDrive quota exhaustion"
            };
        }
        if (before_upload_return) {
            auto callback = std::move(before_upload_return);
            before_upload_return = {};
            callback();
        }
        return {
            .id = remote_id.value_or(
                "uploaded-" + std::to_string(upload_count)
            ),
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "uploaded-etag-" + std::to_string(upload_count),
            .parent_id = "root-id",
            .remote_path = remote_path,
            .last_modified = "2026-10-04T09:00:00Z",
            .size = static_cast<std::int64_t>(
                std::filesystem::file_size(source)
            ),
        };
    }

    [[nodiscard]] onedrive::graph::RemoteItem create_directory(
        const std::string& remote_path
    ) const {
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
                "remote_quota",
                "simulated OneDrive quota exhaustion"
            };
        }
        if (before_directory_return) {
            auto callback = std::move(before_directory_return);
            before_directory_return = {};
            callback();
        }
        return {
            .id = "directory-" + std::to_string(directory_create_count),
            .name =
                std::filesystem::path{remote_path}.filename().string(),
            .etag = "directory-etag-" +
                std::to_string(directory_create_count),
            .parent_id = "root-id",
            .remote_path = remote_path,
            .directory = true,
        };
    }

    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag
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
            .name =
                std::filesystem::path{destination_path}.filename().string(),
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
    std::string upload_resource_error_path;
    bool directory_conflict{false};
    std::string directory_resource_error_path;
    bool delete_conflict{false};
    bool move_conflict{false};
    bool moved_item_directory{false};
    bool fail_after_upload_checkpoint{false};
    std::chrono::milliseconds download_delay{0};
    int downloads_started_before_failure{0};
    int checkpoints_before_failure{0};
    std::size_t cancellation_checkpoint{0};
    mutable std::atomic_int download_count{0};
    mutable int upload_count{0};
    mutable int directory_create_count{0};
    mutable std::vector<std::string> uploaded_paths;
    mutable std::vector<std::string> created_directory_paths;
    mutable std::vector<std::pair<std::string, std::string>> deleted_items;
    mutable std::vector<std::pair<std::string, std::string>>
        moved_remote_items;
    mutable std::vector<std::string> remote_mutations;
    mutable std::vector<
        std::optional<onedrive::graph::UploadSession>
    > upload_sessions;
    mutable std::atomic_int checkpoint_count{0};
    mutable std::atomic_int active_downloads{0};
    mutable std::atomic_int maximum_concurrent_downloads{0};
    mutable std::atomic_uint64_t last_download_offset{0};
    mutable std::vector<std::optional<std::string>> delta_requests;
};

class FakeItemStore final {
public:
    void open() {}

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
        if (delta.replace_drive_items) {
            items.clear();
            blocked.clear();
        }
        for (const auto& remote_id : delta.removals) {
            items.erase(remote_id);
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
                suppression.local_path.lexically_normal().string(),
                suppression
            );
        }
        for (const auto& remote_id : delta.blocked_removals) {
            std::erase_if(
                blocked,
                [&remote_id](const auto& item) {
                    return item.remote_id == remote_id;
                }
            );
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

    void save_pending_download(
        onedrive::storage::PendingDownload download
    ) {
        const std::scoped_lock lock{mutex};
        pending.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_pending_download(
        const std::string&,
        const std::string& remote_id
    ) {
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

    void save_partial_download(
        onedrive::storage::PartialDownload download
    ) {
        const std::scoped_lock lock{mutex};
        partials.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_partial_download(
        const std::string&,
        const std::string& remote_id
    ) {
        const std::scoped_lock lock{mutex};
        partials.erase(remote_id);
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(
        const std::string&,
        const std::string& remote_id
    ) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = partials.find(remote_id);
        return iterator == partials.end() ?
                   std::nullopt :
                   std::optional<onedrive::storage::PartialDownload>{
                       iterator->second
                   };
    }

    void save_pending_upload(onedrive::storage::PendingUpload upload) {
        const std::scoped_lock lock{mutex};
        if (fail_upload_checkpoint_save && !upload.upload_url.empty()) {
            throw std::runtime_error{
                "simulated upload checkpoint persistence failure"
            };
        }
        pending_uploads_by_path.insert_or_assign(
            upload.remote_path,
            std::move(upload)
        );
    }

    void remove_pending_upload(
        const std::string&,
        const std::string& remote_path
    ) {
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
        pending_deletes_by_id.insert_or_assign(
            deletion.remote_id,
            std::move(deletion)
        );
    }

    void remove_pending_delete(
        const std::string&,
        const std::string& remote_id
    ) {
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
            const bool descendant =
                path.size() > deletion.remote_path.size() &&
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

    void save_pending_remote_move(
        onedrive::storage::PendingRemoteMove move
    ) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_remote_move_save) {
            throw std::runtime_error{
                "simulated pending remote move persistence failure"
            };
        }
        pending_remote_moves_by_id.insert_or_assign(
            move.remote_id,
            std::move(move)
        );
    }

    void remove_pending_remote_move(
        const std::string&,
        const std::string& remote_id
    ) {
        const std::scoped_lock lock{mutex};
        pending_remote_moves_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingRemoteMove> result;
        for (const auto& [remote_id, move] :
             pending_remote_moves_by_id) {
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
            throw std::runtime_error{
                "simulated remote move commit failure"
            };
        }
        if (move.directory) {
            for (auto& [remote_id, state] : items) {
                if (remote_id == move.remote_id) {
                    continue;
                }
                const auto relative_remote =
                    std::filesystem::path{state.remote_path}.
                        lexically_relative(move.source_remote_path);
                const auto relative_local =
                    state.local_path.lexically_relative(
                        move.source_local_path
                    );
                if (!relative_remote.empty() &&
                    !relative_remote.native().starts_with("..") &&
                    relative_remote == relative_local) {
                    state.remote_path =
                        (std::filesystem::path{
                             move.destination_remote_path
                         } / relative_remote).generic_string();
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
        pending_moves_by_id.insert_or_assign(
            move.remote_id,
            std::move(move)
        );
    }

    void remove_pending_move(
        const std::string&,
        const std::string& remote_id
    ) {
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
        for (const auto& [path, suppression] :
             upload_suppressions_by_path) {
            static_cast<void>(path);
            result.push_back(suppression);
        }
        return result;
    }

    void remove_upload_suppression(
        const std::string&,
        const std::filesystem::path& local_path
    ) {
        const std::scoped_lock lock{mutex};
        upload_suppressions_by_path.erase(
            local_path.lexically_normal().string()
        );
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem> blocked_items(
        const std::string&
    ) const {
        const std::scoped_lock lock{mutex};
        return blocked;
    }

    bool reset(const std::string&) {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) {
        return {};
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const {
        return saved_delta_link;
    }

    [[nodiscard]] std::optional<std::string> sync_filter_fingerprint(
        const std::string&
    ) const {
        return saved_sync_filter_fingerprint;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState> find(
        const std::string&,
        const std::string& remote_id
    ) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = items.find(remote_id);
        return iterator == items.end() ?
                   std::nullopt :
                   std::optional<onedrive::storage::ItemState>{
                       iterator->second
                   };
    }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock{mutex};
        return items.size();
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState> drive_items(
        const std::string&
    ) const {
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
    std::unordered_map<std::string, onedrive::storage::PartialDownload> partials;
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
    bool fail_commit_delete{false};
    bool fail_commit_remote_move{false};
    bool fail_pending_remote_move_save{false};
    bool fail_upload_checkpoint_save{false};
    bool fail_apply_delta{false};
    mutable std::mutex mutex;
};

class FakeMetrics final {
public:
    void record_sync_run(
        bool success,
        std::chrono::duration<double>
    ) noexcept {
        last_success = success;
    }

    bool last_success{false};
};

using onedrive::test::fail;

onedrive::config::Config config_for(
    const std::filesystem::path& root,
    bool dry_run
) {
    auto config = onedrive::config::Config::defaults();
    config.sync_directory = root;
    config.state_directory = root.parent_path() / "state";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
    config.upload = false;
    config.dry_run = dry_run;
    return config;
}

onedrive::graph::RemoteItem file(
    std::string id,
    std::string path,
    std::int64_t size
) {
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

onedrive::storage::ItemState tracked_item(
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
        .local_modified_ticks = directory ?
            0 :
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::filesystem::last_write_time(local_path).
                    time_since_epoch()
            ).count(),
        .directory = directory,
    };
}

onedrive::graph::RemoteItem deleted_item(std::string id) {
    return {
        .id = std::move(id),
        .deleted = true,
    };
}

int test_dry_run_and_success() {
    TemporaryDirectory temporary;
    const auto dry_root = temporary.path() / "dry";
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("file", "Documents/file.txt", 4)};
    dry_graph.contents["file"] = "data";
    FakeItemStore dry_items;
    FakeMetrics dry_metrics;
    const auto dry_config = config_for(dry_root, true);
    if (onedrive::sync::SyncEngine{
            dry_config,
            dry_graph,
            dry_items,
            dry_metrics
        }.synchronize() != 0 ||
        dry_graph.download_count != 0 || dry_items.apply_count != 0 ||
        std::filesystem::exists(dry_root)) {
        return fail("dry run modified files or synchronization state");
    }

    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
        file("file", "Documents/file.txt", 4),
    };
    graph.contents["file"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    std::ostringstream progress_output;
    std::ostringstream progress_error;
    const onedrive::cli::Console console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::text,
        },
        progress_output,
        progress_error
    };
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics,
            &console
        }.synchronize() != 0 ||
        graph.download_count != 1 || items.upsert_count != 1 ||
        items.apply_count != 1 || !metrics.last_success ||
        !progress_output.str().contains(
            "DL: 0/1 files, 50% (2 B/4 B)"
        ) ||
        !progress_output.str().contains(
            "Done: 1/1 files, 100% (4 B/4 B)"
        ) ||
        !progress_error.str().empty()) {
        return fail("successful download did not commit synchronization state");
    }
    std::ifstream input{root / "Documents/file.txt", std::ios::binary};
    std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (contents != "data" || items.applied_delta.upserts.size() != 2 ||
        !items.applied_delta.replace_drive_items ||
        items.applied_delta.upserts[1].local_size != 4 ||
        items.applied_delta.upserts[1].local_modified_ticks == 0) {
        return fail("downloaded file or local snapshot was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_remote_content_tag_strategy() {
    TemporaryDirectory temporary;
    const auto reused_root = temporary.path() / "ctag-reused";
    std::filesystem::create_directories(reused_root);
    {
        std::ofstream output{reused_root / "file.txt"};
        output << "data";
    }

    auto previous = tracked_item(reused_root, "file", "file.txt");
    previous.etag = "old-etag";
    previous.ctag = "content-version";
    auto metadata_change = file("file", "file.txt", 4);
    metadata_change.etag = "new-etag";
    metadata_change.ctag = "content-version";
    metadata_change.last_modified = "2026-10-05T02:00:00Z";

    FakeGraphClient reused_graph;
    reused_graph.changes = {metadata_change};
    FakeItemStore reused_items;
    reused_items.saved_delta_link = "saved";
    reused_items.items.emplace("file", previous);
    FakeMetrics reused_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(reused_root, false),
            reused_graph,
            reused_items,
            reused_metrics
        }.synchronize() != 0 ||
        reused_graph.download_count != 0 ||
        reused_items.applied_delta.upserts.size() != 1 ||
        reused_items.applied_delta.upserts[0].etag != "new-etag" ||
        reused_items.applied_delta.upserts[0].ctag != "content-version") {
        return fail("unchanged cTag did not reuse local file content");
    }

    const auto changed_root = temporary.path() / "ctag-changed";
    std::filesystem::create_directories(changed_root);
    {
        std::ofstream output{changed_root / "file.txt"};
        output << "data";
    }
    auto changed_previous = tracked_item(changed_root, "file", "file.txt");
    changed_previous.etag = "old-etag";
    changed_previous.ctag = "old-content";
    auto content_change = file("file", "file.txt", 4);
    content_change.etag = "new-etag";
    content_change.ctag = "new-content";

    FakeGraphClient changed_graph;
    changed_graph.changes = {content_change};
    changed_graph.contents["file"] = "next";
    FakeItemStore changed_items;
    changed_items.saved_delta_link = "saved";
    changed_items.items.emplace("file", changed_previous);
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics
        }.synchronize() != 0 ||
        changed_graph.download_count != 1) {
        return fail("changed cTag did not download remote content");
    }

    const auto missing_root = temporary.path() / "ctag-missing";
    std::filesystem::create_directories(missing_root);
    {
        std::ofstream output{missing_root / "file.txt"};
        output << "data";
    }
    auto missing_previous = tracked_item(missing_root, "file", "file.txt");
    missing_previous.etag = "old-etag";
    missing_previous.ctag = "old-content";
    auto missing_ctag = file("file", "file.txt", 4);
    missing_ctag.etag = "new-etag";

    FakeGraphClient missing_graph;
    missing_graph.changes = {missing_ctag};
    missing_graph.contents["file"] = "next";
    FakeItemStore missing_items;
    missing_items.saved_delta_link = "saved";
    missing_items.items.emplace("file", missing_previous);
    FakeMetrics missing_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(missing_root, false),
            missing_graph,
            missing_items,
            missing_metrics
        }.synchronize() != 0 ||
        missing_graph.download_count != 1) {
        return fail("missing cTag bypassed the eTag fallback");
    }
    return EXIT_SUCCESS;
}

int test_selective_sync_refreshes_delta_state() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    const auto sync_list = temporary.path() / "sync_list";
    {
        std::ofstream output{sync_list};
        output << "/Documents/\n";
    }

    FakeGraphClient graph;
    graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
        file("root-file", "root-file.txt", 4),
        file("included", "Documents/included.txt", 4),
        file("excluded", "Pictures/excluded.txt", 7),
    };
    graph.contents["included"] = "data";
    graph.contents["root-file"] = "root";
    graph.contents["excluded"] = "ignored";
    FakeItemStore items;
    items.saved_delta_link = "https://graph.example.test/old-delta";
    items.saved_sync_filter_fingerprint = "old-filter";
    std::filesystem::create_directories(root / "Pictures");
    {
        std::ofstream output{root / "Pictures" / "excluded.txt"};
        output << "ignored";
    }
    items.items.emplace(
        "excluded",
        tracked_item(root, "excluded", "Pictures/excluded.txt")
    );
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_list = sync_list;

    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != 1 ||
        items.applied_delta.upserts.size() != 2 ||
        !items.applied_delta.replace_drive_items ||
        items.applied_delta.sync_filter_fingerprint.empty() ||
        !std::filesystem::exists(root / "Pictures/excluded.txt")) {
        return fail(
            "changed selective sync rules did not force a filtered full delta"
        );
    }

    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;
    graph.delta_requests.clear();
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{
                "https://graph.example.test/delta"
            }) {
        return fail(
            "unchanged selective sync rules did not reuse the delta cursor"
        );
    }

    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;
    graph.delta_requests.clear();
    config.sync_root_files = true;
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != 2 ||
        !std::filesystem::is_regular_file(root / "root-file.txt") ||
        !items.applied_delta.replace_drive_items) {
        return fail(
            "enabling root files did not force and apply a filtered full delta"
        );
    }
    return EXIT_SUCCESS;
}

int test_selective_sync_remote_moves() {
    TemporaryDirectory temporary;
    const auto sync_list = temporary.path() / "sync_list";
    {
        std::ofstream output{sync_list};
        output << "/Documents/\n";
    }
    const auto root = temporary.path() / "file-move";
    std::filesystem::create_directories(root / "Documents");
    {
        std::ofstream output{root / "Documents" / "A.txt"};
        output << "data";
    }
    FakeItemStore items;
    items.items.emplace(
        "selective-move",
        tracked_item(
            root,
            "selective-move",
            "Documents/A.txt"
        )
    );
    FakeGraphClient graph;
    graph.changes = {
        file("selective-move", "Archive/A.txt", 4),
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_list = sync_list;
    config.upload = true;
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        !std::filesystem::exists(root / "Documents" / "A.txt") ||
        std::filesystem::exists(root / "Archive" / "A.txt") ||
        graph.upload_count != 0 ||
        items.upload_suppressions_by_path.size() != 1 ||
        items.items.contains("selective-move")) {
        return fail(
            "move from included to excluded path was not retained safely"
        );
    }
    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;

    {
        std::ofstream output{
            root / "Documents" / "A.txt",
            std::ios::trunc
        };
        output << "user";
    }
    graph.changes.clear();
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.upload_count != 0 ||
        items.upload_suppressions_by_path.size() != 1) {
        return fail(
            "modified selectively retained file was uploaded unexpectedly"
        );
    }

    graph.changes = {
        file("selective-move", "Documents/B.txt", 4),
    };
    graph.contents["selective-move"] = "data";
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.download_count != 1 ||
        graph.upload_count != 0 ||
        !std::filesystem::exists(root / "Documents" / "A.txt") ||
        !std::filesystem::exists(root / "Documents" / "B.txt") ||
        items.upload_suppressions_by_path.size() != 1) {
        return fail(
            "move from excluded to included path lost retained protection"
        );
    }

    std::filesystem::rename(
        root / "Documents" / "A.txt",
        root / "retained-A.txt"
    );
    {
        std::ofstream output{root / "Documents" / "A.txt"};
        output << "new!";
    }
    graph.changes.clear();
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.upload_count != 1 ||
        graph.uploaded_paths !=
            std::vector<std::string>{"Documents/A.txt"} ||
        !items.upload_suppressions_by_path.empty()) {
        return fail(
            "replacement of selectively retained file stayed suppressed"
        );
    }

    const auto directory_root = temporary.path() / "directory-move";
    std::filesystem::create_directories(
        directory_root / "Documents" / "Project"
    );
    {
        std::ofstream output{
            directory_root / "Documents" / "Project" / "one.txt"
        };
        output << "data";
    }
    {
        std::ofstream output{
            directory_root / "Documents" / "Project" / "two.txt"
        };
        output << "data";
    }
    FakeItemStore directory_items;
    directory_items.items.emplace(
        "selective-directory",
        tracked_item(
            directory_root,
            "selective-directory",
            "Documents/Project",
            true
        )
    );
    directory_items.items.emplace(
        "selective-child-one",
        tracked_item(
            directory_root,
            "selective-child-one",
            "Documents/Project/one.txt"
        )
    );
    directory_items.items.emplace(
        "selective-child-two",
        tracked_item(
            directory_root,
            "selective-child-two",
            "Documents/Project/two.txt"
        )
    );
    FakeGraphClient directory_graph;
    directory_graph.changes = {
        {
            .id = "selective-directory",
            .name = "Project",
            .etag = "directory-etag-2",
            .parent_id = "archive",
            .remote_path = "Archive/Project",
            .directory = true,
        },
        file(
            "selective-child-one",
            "Archive/Project/one.txt",
            4
        ),
        file(
            "selective-child-two",
            "Archive/Project/two.txt",
            4
        ),
    };
    FakeMetrics directory_metrics;
    auto directory_config = config_for(directory_root, false);
    directory_config.sync_list = sync_list;
    directory_config.upload = true;
    if (onedrive::sync::SyncEngine{
            directory_config,
            directory_graph,
            directory_items,
            directory_metrics
        }.synchronize() != 0 ||
        directory_graph.upload_count != 0 ||
        directory_items.upload_suppressions_by_path.size() != 2 ||
        !directory_items.items.empty() ||
        !std::filesystem::exists(
            directory_root / "Documents" / "Project" / "one.txt"
        ) ||
        !std::filesystem::exists(
            directory_root / "Documents" / "Project" / "two.txt"
        )) {
        return fail(
            "directory move outside selective sync did not retain descendants"
        );
    }

    const auto dry_root = temporary.path() / "dry-move";
    std::filesystem::create_directories(dry_root / "Documents");
    {
        std::ofstream output{dry_root / "Documents" / "A.txt"};
        output << "data";
    }
    FakeItemStore dry_items;
    dry_items.items.emplace(
        "dry-selective-move",
        tracked_item(
            dry_root,
            "dry-selective-move",
            "Documents/A.txt"
        )
    );
    FakeGraphClient dry_graph;
    dry_graph.changes = {
        file("dry-selective-move", "Archive/A.txt", 4),
    };
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.sync_list = sync_list;
    dry_config.upload = true;
    if (onedrive::sync::SyncEngine{
            dry_config,
            dry_graph,
            dry_items,
            dry_metrics
        }.synchronize() != 0 ||
        dry_items.apply_count != 0 ||
        !dry_items.upload_suppressions_by_path.empty() ||
        dry_graph.upload_count != 0 ||
        !std::filesystem::exists(dry_root / "Documents" / "A.txt")) {
        return fail("selective move dry run changed local or durable state");
    }
    return EXIT_SUCCESS;
}

int test_malware_file_is_blocked_without_overwriting_local_data() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "reported-malware.exe";
    {
        std::ofstream output{destination, std::ios::binary};
        output << "local data";
    }

    FakeGraphClient graph;
    auto malware = file("malware", "reported-malware.exe", 4);
    malware.malware = true;
    graph.changes = {malware};
    graph.contents["malware"] = "evil";
    FakeItemStore items;
    FakeMetrics metrics;
    if (onedrive::sync::SyncEngine{
            config_for(root, false),
            graph,
            items,
            metrics
        }.synchronize() != 2 ||
        graph.download_count != 0 ||
        items.applied_delta.upserts.size() != 0 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].reason_code !=
            "malware_detected" ||
        !metrics.last_success) {
        return fail("Graph malware item was not isolated from downloads");
    }
    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (contents != "local data") {
        return fail("Graph malware item overwrote an existing local file");
    }
    return EXIT_SUCCESS;
}

int test_failure_and_conflict() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("first", "first.txt", 4),
        file("second", "second.txt", 4),
    };
    graph.contents["first"] = "data";
    graph.failing_id = "second";
    FakeItemStore items;
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize()
        );
        return fail("download failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (items.apply_count != 0 || items.upsert_count != 1 ||
        !std::filesystem::exists(root / "first.txt") ||
        std::filesystem::exists(root / "second.txt") || metrics.last_success) {
        return fail("failed download advanced state or left an invalid file");
    }

    const auto independent_root = temporary.path() / "independent";
    FakeGraphClient independent_graph;
    independent_graph.changes = {
        file("failed-first", "failed-first.txt", 4),
        file("completed-second", "completed-second.txt", 4),
    };
    independent_graph.contents["completed-second"] = "data";
    independent_graph.failing_id = "failed-first";
    independent_graph.downloads_started_before_failure = 2;
    FakeItemStore independent_items;
    FakeMetrics independent_metrics;
    auto independent_config = config_for(independent_root, false);
    independent_config.download_concurrency = 2;
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{
                independent_config,
                independent_graph,
                independent_items,
                independent_metrics
            }.synchronize()
        );
        return fail("concurrent download failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (independent_items.upsert_count != 1 ||
        !std::filesystem::exists(
            independent_root / "completed-second.txt"
        ) ||
        std::filesystem::exists(
            independent_root / "failed-first.txt"
        ) ||
        independent_metrics.last_success) {
        return fail(
            "an independently completed download was discarded after failure"
        );
    }

    const auto changed_root = temporary.path() / "changed-during-download";
    FakeGraphClient changed_graph;
    changed_graph.changes = {
        file("changed", "changed.txt", 4),
    };
    changed_graph.contents["changed"] = "data";
    changed_graph.before_download_write =
        [destination = changed_root / "changed.txt"](const std::string&) {
            std::ofstream output{destination};
            output << "user data";
        };
    FakeItemStore changed_items;
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics
        }.synchronize() != 2 ||
        changed_items.upsert_count != 0 ||
        changed_items.apply_count != 1 ||
        changed_items.applied_delta.blocked_upserts.size() != 1 ||
        changed_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !changed_items.pending.empty() ||
        !changed_metrics.last_success) {
        return fail("download-time local modification was not isolated");
    }
    {
        std::ifstream input{changed_root / "changed.txt"};
        std::string contents{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (contents != "user data") {
            return fail("download overwrote a file created during transfer");
        }
    }

    const auto conflict_root = temporary.path() / "conflict";
    std::filesystem::create_directories(conflict_root);
    {
        std::ofstream output{conflict_root / "existing.txt"};
        output << "user data";
    }
    FakeGraphClient conflict_graph;
    conflict_graph.changes = {file("existing", "existing.txt", 4)};
    conflict_graph.contents["existing"] = "data";
    FakeItemStore conflict_items;
    FakeMetrics conflict_metrics;
    const auto conflict_config = config_for(conflict_root, false);
    if (onedrive::sync::SyncEngine{
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics
        }.synchronize() != 2 ||
        conflict_graph.download_count != 0 ||
        conflict_items.apply_count != 1 ||
        conflict_items.applied_delta.blocked_upserts.size() != 1 ||
        conflict_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !conflict_metrics.last_success) {
        return fail("local conflict was not isolated and persisted");
    }

    const auto backup_root = temporary.path() / "backup-conflict";
    std::filesystem::create_directories(backup_root);
    {
        std::ofstream output{backup_root / "existing.txt"};
        output << "user data";
    }
    FakeGraphClient backup_graph;
    backup_graph.changes = {file("existing", "existing.txt", 4)};
    backup_graph.contents["existing"] = "data";
    FakeItemStore backup_items;
    FakeMetrics backup_metrics;
    auto backup_config = config_for(backup_root, false);
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            backup_config,
            backup_graph,
            backup_items,
            backup_metrics
        }.synchronize() != 0 ||
        backup_graph.download_count != 1 ||
        backup_items.upsert_count != 1 ||
        !backup_items.applied_delta.blocked_upserts.empty() ||
        !backup_metrics.last_success) {
        return fail("safeBackup mode did not resolve a local conflict");
    }
    std::filesystem::path preserved;
    for (const auto& entry :
         std::filesystem::directory_iterator{backup_root}) {
        if (entry.path().filename().string().starts_with(
                "existing.safeBackup-"
            )) {
            preserved = entry.path();
        }
    }
    std::ifstream installed_input{backup_root / "existing.txt"};
    std::ifstream preserved_input{preserved};
    const std::string installed{
        std::istreambuf_iterator<char>{installed_input},
        std::istreambuf_iterator<char>{}
    };
    const std::string preserved_contents{
        std::istreambuf_iterator<char>{preserved_input},
        std::istreambuf_iterator<char>{}
    };
    if (preserved.empty() || installed != "data" ||
        preserved_contents != "user data") {
        return fail("safeBackup mode did not preserve conflicting content");
    }

    const auto late_backup_root =
        temporary.path() / "backup-created-during-download";
    FakeGraphClient late_backup_graph;
    late_backup_graph.changes = {
        file("late-backup", "late-backup.txt", 4),
    };
    late_backup_graph.contents["late-backup"] = "data";
    late_backup_graph.before_download_write =
        [destination =
             late_backup_root / "late-backup.txt"](const std::string&) {
            std::ofstream output{destination};
            output << "user data";
        };
    FakeItemStore late_backup_items;
    FakeMetrics late_backup_metrics;
    auto late_backup_config = config_for(late_backup_root, false);
    late_backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            late_backup_config,
            late_backup_graph,
            late_backup_items,
            late_backup_metrics
        }.synchronize() != 0 ||
        late_backup_items.upsert_count != 1 ||
        !late_backup_items.applied_delta.blocked_upserts.empty() ||
        !late_backup_metrics.last_success) {
        return fail(
            "safeBackup mode did not preserve a download-time conflict"
        );
    }
    bool preserved_late_change = false;
    for (const auto& entry :
         std::filesystem::directory_iterator{late_backup_root}) {
        if (entry.path().filename().string().starts_with(
                "late-backup.safeBackup-"
            )) {
            std::ifstream input{entry.path()};
            const std::string contents{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}
            };
            preserved_late_change = contents == "user data";
        }
    }
    if (!preserved_late_change) {
        return fail("download-time local content was not backed up");
    }

    const auto symlink_root = temporary.path() / "symlink-root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(symlink_root);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(
        outside,
        symlink_root / "linked"
    );
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {file("linked", "linked/file.txt", 4)};
    symlink_graph.contents["linked"] = "data";
    FakeItemStore symlink_items;
    FakeMetrics symlink_metrics;
    const auto symlink_config = config_for(symlink_root, false);
    if (onedrive::sync::SyncEngine{
            symlink_config,
            symlink_graph,
            symlink_items,
            symlink_metrics
        }.synchronize() != 2 ||
        symlink_graph.download_count != 0 ||
        symlink_items.apply_count != 1 ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict" ||
        std::filesystem::exists(outside / "file.txt") ||
        !symlink_metrics.last_success) {
        return fail("symbolic link conflict was not isolated safely");
    }

    const auto layout_root = temporary.path() / "layout-root";
    const auto layout_outside = temporary.path() / "layout-outside";
    std::filesystem::create_directories(layout_root);
    std::filesystem::create_directories(layout_outside);
    std::filesystem::create_directory_symlink(
        layout_outside,
        layout_root / "accounts"
    );
    FakeGraphClient layout_graph;
    FakeItemStore layout_items;
    FakeMetrics layout_metrics;
    const auto layout_config = config_for(
        layout_root / "accounts/Test-User/drives/Test-Drive",
        false
    );
    bool layout_symlink_rejected = false;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            layout_config,
            layout_graph,
            layout_items,
            layout_metrics
        }.synchronize());
    } catch (const std::runtime_error& error) {
        layout_symlink_rejected = std::string_view{error.what()}.contains(
            "contains a symbolic link"
        );
    }
    if (!layout_symlink_rejected ||
        std::filesystem::exists(layout_outside / "Test-User") ||
        layout_metrics.last_success) {
        return fail("symbolic link in account data layout was accepted");
    }
    return EXIT_SUCCESS;
}

int test_blocked_items_continue_and_retry() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("invalid", std::string(256, 'x'), 4),
        file("good", "good.txt", 4),
    };
    graph.contents["good"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    if (onedrive::sync::SyncEngine{
            config_for(root, false),
            graph,
            items,
            metrics
        }.synchronize() != 2 ||
        graph.download_count != 1 ||
        !std::filesystem::exists(root / "good.txt") ||
        items.apply_count != 1 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].remote_id != "invalid" ||
        items.applied_delta.upserts.size() != 1 ||
        !metrics.last_success) {
        return fail("invalid filename prevented an independent download");
    }

    const auto parent_root = temporary.path() / "parent-conflict";
    std::filesystem::create_directories(parent_root);
    {
        std::ofstream output{parent_root / "Documents"};
        output << "local file";
    }
    FakeGraphClient parent_graph;
    parent_graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
        file("child", "Documents/child.txt", 4),
        file("independent", "independent.txt", 4),
    };
    parent_graph.contents["independent"] = "data";
    FakeItemStore parent_items;
    FakeMetrics parent_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(parent_root, false),
            parent_graph,
            parent_items,
            parent_metrics
        }.synchronize() != 2 ||
        parent_graph.download_count != 1 ||
        !std::filesystem::exists(parent_root / "independent.txt") ||
        parent_items.applied_delta.blocked_upserts.size() != 2 ||
        parent_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict" ||
        parent_items.applied_delta.blocked_upserts[1].reason_code !=
            "blocked_by_parent") {
        return fail("blocked directory did not isolate its descendants");
    }

    const auto retry_root = temporary.path() / "retry";
    FakeGraphClient retry_graph;
    retry_graph.contents["retry"] = "data";
    FakeItemStore retry_items;
    retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "retry",
            .parent_id = "root",
            .name = "retry.txt",
            .etag = "retry-etag",
            .remote_path = "retry.txt",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "local_modification",
            .reason_message = "previous conflict",
            .attempt_count = 1,
        },
    };
    FakeMetrics retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(retry_root, false),
            retry_graph,
            retry_items,
            retry_metrics
        }.synchronize() != 0 ||
        retry_graph.download_count != 1 ||
        !std::filesystem::exists(retry_root / "retry.txt") ||
        retry_items.applied_delta.blocked_upserts.size() != 0 ||
        retry_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"retry"} ||
        !retry_metrics.last_success) {
        return fail("persisted blocked item was not retried and cleared");
    }

    const auto hash_retry_root = temporary.path() / "hash-retry";
    FakeGraphClient hash_retry_graph;
    hash_retry_graph.contents["hash-retry"] = "data";
    FakeItemStore hash_retry_items;
    hash_retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    hash_retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "hash-retry",
            .parent_id = "root",
            .name = "hash-retry.txt",
            .etag = "retry-etag",
            .remote_path = "hash-retry.txt",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "local_modification",
            .reason_message = "previous conflict",
            .attempt_count = 1,
            .content_hash = onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value =
                    "000000000000000000000000000000000000000000000000"
                    "0000000000000000",
            },
        },
    };
    FakeMetrics hash_retry_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(hash_retry_root, false),
            hash_retry_graph,
            hash_retry_items,
            hash_retry_metrics
        }.synchronize());
        return fail("blocked retry ignored its persisted content hash");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(hash_retry_root / "hash-retry.txt") ||
        !hash_retry_items.partials.empty() ||
        hash_retry_metrics.last_success) {
        return fail("failed blocked hash retry retained resumable state");
    }

    const auto malware_retry_root = temporary.path() / "malware-retry";
    FakeGraphClient malware_retry_graph;
    malware_retry_graph.contents["malware-retry"] = "evil";
    FakeItemStore malware_retry_items;
    malware_retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    malware_retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "malware-retry",
            .parent_id = "root",
            .name = "malware-retry.exe",
            .etag = "retry-etag",
            .remote_path = "malware-retry.exe",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "malware_detected",
            .reason_message =
                "Microsoft Graph marked the remote file as malware",
            .attempt_count = 1,
        },
    };
    FakeMetrics malware_retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(malware_retry_root, false),
            malware_retry_graph,
            malware_retry_items,
            malware_retry_metrics
        }.synchronize() != 2 ||
        malware_retry_graph.download_count != 0 ||
        malware_retry_items.applied_delta.blocked_upserts.size() != 1 ||
        malware_retry_items.applied_delta.blocked_upserts[0].reason_code !=
            "malware_detected" ||
        !malware_retry_metrics.last_success) {
        return fail("persisted malware item was retried as a download");
    }

    const auto cleared_root = temporary.path() / "malware-cleared";
    FakeGraphClient cleared_graph;
    cleared_graph.changes = {
        file("malware-cleared", "malware-cleared.txt", 4),
    };
    cleared_graph.contents["malware-cleared"] = "data";
    FakeItemStore cleared_items;
    cleared_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    cleared_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "malware-cleared",
            .parent_id = "root",
            .name = "malware-cleared.txt",
            .etag = "old-malware-etag",
            .remote_path = "malware-cleared.txt",
            .last_modified = "2026-10-01T00:00:00Z",
            .size = 4,
            .reason_code = "malware_detected",
            .reason_message =
                "Microsoft Graph marked the remote file as malware",
            .attempt_count = 1,
        },
    };
    FakeMetrics cleared_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cleared_root, false),
            cleared_graph,
            cleared_items,
            cleared_metrics
        }.synchronize() != 0 ||
        cleared_graph.download_count != 1 ||
        !std::filesystem::exists(cleared_root / "malware-cleared.txt") ||
        !cleared_items.applied_delta.blocked_upserts.empty() ||
        cleared_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"malware-cleared"} ||
        !cleared_metrics.last_success) {
        return fail("cleared Graph malware marker did not resume downloading");
    }
    return EXIT_SUCCESS;
}

int test_invalid_delta_cursor_restarts_full_query() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.reject_saved_cursor = true;
    graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
    };
    FakeItemStore items;
    items.saved_delta_link =
        "https://graph.example.test/delta?token=expired";
    FakeMetrics metrics;

    if (onedrive::sync::SyncEngine{
            config_for(root, false),
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{
                items.saved_delta_link,
                std::nullopt,
            } ||
        items.apply_count != 1 || !items.applied_delta.replace_drive_items ||
        items.applied_delta.upserts.size() != 1 || !metrics.last_success) {
        return fail(
            "invalid delta cursor did not restart a replacing full query"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_deletions() {
    TemporaryDirectory temporary;

    const auto unchanged_root = temporary.path() / "unchanged";
    std::filesystem::create_directories(unchanged_root);
    {
        std::ofstream output{unchanged_root / "deleted.txt"};
        output << "data";
    }
    FakeGraphClient unchanged_graph;
    unchanged_graph.changes = {deleted_item("deleted")};
    FakeItemStore unchanged_items;
    unchanged_items.saved_delta_link = "saved";
    unchanged_items.items.emplace(
        "deleted",
        tracked_item(unchanged_root, "deleted", "deleted.txt")
    );
    FakeMetrics unchanged_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(unchanged_root, false),
            unchanged_graph,
            unchanged_items,
            unchanged_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(unchanged_root / "deleted.txt") ||
        unchanged_items.applied_delta.removals !=
            std::vector<std::string>{"deleted"} ||
        unchanged_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"deleted"} ||
        !unchanged_items.applied_delta.blocked_upserts.empty() ||
        !unchanged_metrics.last_success) {
        return fail("unchanged remote deletion was not executed safely");
    }

    const auto dry_root = temporary.path() / "dry";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "deleted.txt"};
        output << "data";
    }
    FakeGraphClient dry_graph;
    dry_graph.changes = {deleted_item("deleted")};
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace(
        "deleted",
        tracked_item(dry_root, "deleted", "deleted.txt")
    );
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true),
            dry_graph,
            dry_items,
            dry_metrics
        }.synchronize() != 0 ||
        !std::filesystem::exists(dry_root / "deleted.txt") ||
        dry_items.apply_count != 0) {
        return fail("remote deletion dry run changed local state");
    }

    const auto refreshed_root = temporary.path() / "full-refresh";
    std::filesystem::create_directories(refreshed_root);
    {
        std::ofstream output{refreshed_root / "gone.txt"};
        output << "data";
    }
    FakeGraphClient refreshed_graph;
    FakeItemStore refreshed_items;
    refreshed_items.items.emplace(
        "gone",
        tracked_item(refreshed_root, "gone", "gone.txt")
    );
    FakeMetrics refreshed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(refreshed_root, false),
            refreshed_graph,
            refreshed_items,
            refreshed_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(refreshed_root / "gone.txt") ||
        refreshed_items.applied_delta.removals !=
            std::vector<std::string>{"gone"} ||
        !refreshed_items.applied_delta.replace_drive_items) {
        return fail(
            "full remote refresh did not reconcile a disappeared item"
        );
    }

    const auto modified_root = temporary.path() / "modified";
    std::filesystem::create_directories(modified_root);
    {
        std::ofstream output{modified_root / "modified.txt"};
        output << "data";
    }
    FakeItemStore modified_items;
    modified_items.saved_delta_link = "saved";
    modified_items.items.emplace(
        "modified",
        tracked_item(modified_root, "modified", "modified.txt")
    );
    {
        std::ofstream output{modified_root / "modified.txt"};
        output << "user data";
    }
    FakeGraphClient modified_graph;
    modified_graph.changes = {deleted_item("modified")};
    FakeMetrics modified_metrics;
    auto modified_config = config_for(modified_root, false);
    modified_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            modified_config,
            modified_graph,
            modified_items,
            modified_metrics
        }.synchronize() != 2 ||
        !std::filesystem::exists(modified_root / "modified.txt") ||
        !modified_items.applied_delta.removals.empty() ||
        modified_items.applied_delta.blocked_upserts.size() != 1 ||
        !modified_items.applied_delta.blocked_upserts[0].deleted ||
        modified_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !modified_metrics.last_success) {
        return fail("modified remote deletion was not blocked and persisted");
    }
    std::filesystem::remove(modified_root / "modified.txt");
    FakeGraphClient retry_graph;
    FakeItemStore retry_items;
    retry_items.saved_delta_link = "saved-after-deletion";
    retry_items.items = modified_items.items;
    retry_items.blocked =
        modified_items.applied_delta.blocked_upserts;
    FakeMetrics retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(modified_root, false),
            retry_graph,
            retry_items,
            retry_metrics
        }.synchronize() != 0 ||
        retry_items.applied_delta.removals !=
            std::vector<std::string>{"modified"} ||
        retry_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"modified"} ||
        !retry_items.applied_delta.blocked_upserts.empty()) {
        return fail("blocked remote deletion was not retried successfully");
    }

    const auto tree_root = temporary.path() / "tree";
    std::filesystem::create_directories(tree_root / "Folder");
    {
        std::ofstream output{tree_root / "Folder" / "child.txt"};
        output << "data";
    }
    FakeItemStore tree_items;
    tree_items.saved_delta_link = "saved";
    tree_items.items.emplace(
        "folder",
        tracked_item(tree_root, "folder", "Folder", true)
    );
    tree_items.items.emplace(
        "child",
        tracked_item(tree_root, "child", "Folder/child.txt")
    );
    FakeGraphClient tree_graph;
    tree_graph.changes = {
        deleted_item("folder"),
    };
    FakeMetrics tree_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(tree_root, false),
            tree_graph,
            tree_items,
            tree_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(tree_root / "Folder") ||
        tree_items.applied_delta.removals.size() != 2) {
        return fail("remote deletion did not remove children before parents");
    }

    const auto nonempty_root = temporary.path() / "nonempty";
    std::filesystem::create_directories(nonempty_root / "Folder");
    {
        std::ofstream output{nonempty_root / "Folder" / "local.txt"};
        output << "local";
    }
    FakeItemStore nonempty_items;
    nonempty_items.saved_delta_link = "saved";
    nonempty_items.items.emplace(
        "folder",
        tracked_item(nonempty_root, "folder", "Folder", true)
    );
    FakeGraphClient nonempty_graph;
    nonempty_graph.changes = {deleted_item("folder")};
    FakeMetrics nonempty_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nonempty_root, false),
            nonempty_graph,
            nonempty_items,
            nonempty_metrics
        }.synchronize() != 2 ||
        !std::filesystem::exists(nonempty_root / "Folder" / "local.txt") ||
        nonempty_items.applied_delta.blocked_upserts.size() != 1 ||
        nonempty_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("nonempty remotely deleted directory was not blocked");
    }

    const auto symlink_root = temporary.path() / "delete-symlink";
    const auto outside = temporary.path() / "delete-outside.txt";
    std::filesystem::create_directories(symlink_root);
    {
        std::ofstream output{outside};
        output << "data";
    }
    std::filesystem::create_symlink(
        outside,
        symlink_root / "linked.txt"
    );
    FakeItemStore symlink_items;
    symlink_items.saved_delta_link = "saved";
    symlink_items.items.emplace(
        "linked",
        onedrive::storage::ItemState{
            .drive_id = "me",
            .remote_id = "linked",
            .name = "linked.txt",
            .remote_path = "linked.txt",
            .local_path = symlink_root / "linked.txt",
            .size = 4,
            .local_size = 4,
        }
    );
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {deleted_item("linked")};
    FakeMetrics symlink_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(symlink_root, false),
            symlink_graph,
            symlink_items,
            symlink_metrics
        }.synchronize() != 2 ||
        !std::filesystem::is_symlink(symlink_root / "linked.txt") ||
        !std::filesystem::exists(outside) ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote deletion followed or removed a symbolic link");
    }

    const auto missing_root = temporary.path() / "missing";
    FakeItemStore missing_items;
    missing_items.saved_delta_link = "saved";
    missing_items.items.emplace(
        "missing",
        onedrive::storage::ItemState{
            .drive_id = "me",
            .remote_id = "missing",
            .name = "missing.txt",
            .remote_path = "missing.txt",
            .local_path = missing_root / "missing.txt",
            .size = 4,
            .local_size = 4,
        }
    );
    FakeGraphClient missing_graph;
    missing_graph.changes = {deleted_item("missing")};
    FakeMetrics missing_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(missing_root, false),
            missing_graph,
            missing_items,
            missing_metrics
        }.synchronize() != 0 ||
        missing_items.applied_delta.removals !=
            std::vector<std::string>{"missing"}) {
        return fail("already absent remote deletion did not clear its snapshot");
    }

    const auto untracked_root = temporary.path() / "untracked";
    FakeItemStore untracked_items;
    untracked_items.saved_delta_link = "saved";
    FakeGraphClient untracked_graph;
    untracked_graph.changes = {deleted_item("untracked")};
    FakeMetrics untracked_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(untracked_root, false),
            untracked_graph,
            untracked_items,
            untracked_metrics
        }.synchronize() != 0 ||
        untracked_items.applied_delta.removals !=
            std::vector<std::string>{"untracked"}) {
        return fail("untracked remote deletion did not clear stale state");
    }
    return EXIT_SUCCESS;
}

int test_remote_moves() {
    TemporaryDirectory temporary;

    const auto renamed_root = temporary.path() / "renamed";
    std::filesystem::create_directories(renamed_root);
    {
        std::ofstream output{renamed_root / "old.txt"};
        output << "data";
    }
    FakeGraphClient renamed_graph;
    renamed_graph.changes = {file("renamed", "new.txt", 4)};
    FakeItemStore renamed_items;
    renamed_items.saved_delta_link = "saved";
    renamed_items.items.emplace(
        "renamed",
        tracked_item(renamed_root, "renamed", "old.txt")
    );
    FakeMetrics renamed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(renamed_root, false),
            renamed_graph,
            renamed_items,
            renamed_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(renamed_root / "old.txt") ||
        !std::filesystem::exists(renamed_root / "new.txt") ||
        renamed_graph.download_count != 0 ||
        renamed_items.applied_delta.upserts.size() != 1 ||
        renamed_items.applied_delta.upserts[0].remote_path != "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_path !=
            renamed_root / "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_size != 4 ||
        !renamed_items.pending_moves_by_id.empty() ||
        !renamed_metrics.last_success) {
        return fail("remote file rename was not applied locally");
    }

    const auto journal_failure_root = temporary.path() / "move-journal-failure";
    std::filesystem::create_directories(journal_failure_root);
    {
        std::ofstream output{journal_failure_root / "before.txt"};
        output << "data";
    }
    FakeItemStore journal_failure_items;
    journal_failure_items.saved_delta_link = "saved";
    journal_failure_items.items.emplace(
        "journal-failure-move",
        tracked_item(journal_failure_root, "journal-failure-move", "before.txt")
    );
    FakeGraphClient journal_failure_graph;
    FakeMetrics journal_failure_metrics;
    auto journal_failure_config = config_for(journal_failure_root, false);
    journal_failure_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_failure_config,
        journal_failure_graph,
        journal_failure_items,
        journal_failure_metrics
    }
                          .synchronize());
    std::filesystem::rename(
        journal_failure_root / "before.txt", journal_failure_root / "after.txt"
    );
    journal_failure_items.fail_pending_remote_move_save = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            journal_failure_config,
            journal_failure_graph,
            journal_failure_items,
            journal_failure_metrics
        }
                              .synchronize());
        return fail("remote move journal failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!journal_failure_graph.moved_remote_items.empty() ||
        !journal_failure_items.pending_remote_moves_by_id.empty()) {
        return fail("unjournaled remote move reached Microsoft Graph");
    }
    journal_failure_items.fail_pending_remote_move_save = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_failure_config,
        journal_failure_graph,
        journal_failure_items,
        journal_failure_metrics
    }
                          .synchronize());
    if (journal_failure_graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"journal-failure-move", "after.txt"},
            } ||
        !journal_failure_items.pending_remote_moves_by_id.empty()) {
        return fail("remote move did not recover after journal failure");
    }

    const auto recovery_root = temporary.path() / "move-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "old.txt"};
        output << "data";
    }
    FakeGraphClient failed_move_graph;
    failed_move_graph.changes = {
        file("move-recovery", "new.txt", 4),
    };
    FakeItemStore failed_move_items;
    failed_move_items.saved_delta_link = "saved";
    failed_move_items.items.emplace(
        "move-recovery",
        tracked_item(
            recovery_root,
            "move-recovery",
            "old.txt"
        )
    );
    failed_move_items.fail_apply_delta = true;
    FakeMetrics failed_move_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            failed_move_graph,
            failed_move_items,
            failed_move_metrics
        }.synchronize());
        return fail("delta commit failure did not interrupt remote move");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(recovery_root / "old.txt") ||
        !std::filesystem::exists(recovery_root / "new.txt") ||
        failed_move_items.pending_moves_by_id.size() != 1 ||
        failed_move_metrics.last_success) {
        return fail("interrupted remote move did not retain its journal");
    }
    FakeGraphClient recovered_move_graph;
    recovered_move_graph.changes = {
        file("move-recovery", "new.txt", 4),
    };
    FakeItemStore recovered_move_items;
    recovered_move_items.saved_delta_link = "saved";
    recovered_move_items.items = failed_move_items.items;
    recovered_move_items.pending_moves_by_id =
        failed_move_items.pending_moves_by_id;
    FakeMetrics recovered_move_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            recovered_move_graph,
            recovered_move_items,
            recovered_move_metrics
        }.synchronize() != 0 ||
        recovered_move_graph.download_count != 0 ||
        !recovered_move_items.pending_moves_by_id.empty() ||
        !recovered_move_metrics.last_success) {
        return fail("pending remote move was not recovered by inode identity");
    }
    FakeGraphClient deleted_move_graph;
    deleted_move_graph.changes = {
        deleted_item("move-recovery"),
    };
    FakeItemStore deleted_move_items;
    deleted_move_items.saved_delta_link = "saved";
    deleted_move_items.items = failed_move_items.items;
    deleted_move_items.pending_moves_by_id =
        failed_move_items.pending_moves_by_id;
    FakeMetrics deleted_move_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            deleted_move_graph,
            deleted_move_items,
            deleted_move_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(recovery_root / "new.txt") ||
        !deleted_move_items.pending_moves_by_id.empty() ||
        deleted_move_items.applied_delta.removals !=
            std::vector<std::string>{"move-recovery"}) {
        return fail("remotely deleted pending move target was not removed");
    }

    const auto adopted_root = temporary.path() / "adopted";
    std::filesystem::create_directories(adopted_root);
    {
        std::ofstream output{adopted_root / "new.txt"};
        output << "data";
    }
    FakeGraphClient adopted_graph;
    adopted_graph.changes = {file("adopted", "new.txt", 4)};
    FakeItemStore adopted_items;
    adopted_items.saved_delta_link = "saved";
    auto adopted_state =
        tracked_item(adopted_root, "adopted", "new.txt");
    adopted_state.name = "old.txt";
    adopted_state.remote_path = "old.txt";
    adopted_state.local_path = adopted_root / "old.txt";
    adopted_items.items.emplace("adopted", std::move(adopted_state));
    FakeMetrics adopted_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(adopted_root, false),
            adopted_graph,
            adopted_items,
            adopted_metrics
        }.synchronize() != 0 ||
        adopted_graph.download_count != 0 ||
        adopted_items.applied_delta.upserts.size() != 1 ||
        adopted_items.applied_delta.upserts[0].local_path !=
            adopted_root / "new.txt") {
        return fail("interrupted remote move destination was not adopted");
    }

    const auto mismatched_root = temporary.path() / "mismatched-move";
    std::filesystem::create_directories(mismatched_root);
    {
        std::ofstream output{mismatched_root / "new.txt"};
        output << "data";
    }
    FakeGraphClient mismatched_graph;
    mismatched_graph.changes = {
        file("mismatched-move", "new.txt", 4),
    };
    FakeItemStore mismatched_items;
    mismatched_items.saved_delta_link = "saved";
    auto mismatched_state =
        tracked_item(mismatched_root, "mismatched-move", "new.txt");
    mismatched_state.name = "old.txt";
    mismatched_state.remote_path = "old.txt";
    mismatched_state.local_path = mismatched_root / "old.txt";
    mismatched_items.items.emplace(
        "mismatched-move",
        std::move(mismatched_state)
    );
    mismatched_items.pending_moves_by_id.emplace(
        "mismatched-move",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "mismatched-move",
            .source_path = mismatched_root / "old.txt",
            .destination_path = mismatched_root / "new.txt",
            .source_device = 0,
            .source_inode = 0,
        }
    );
    FakeMetrics mismatched_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(mismatched_root, false),
            mismatched_graph,
            mismatched_items,
            mismatched_metrics
        }.synchronize() != 2 ||
        mismatched_items.applied_delta.blocked_upserts.size() != 1 ||
        mismatched_items.applied_delta.blocked_upserts[0].reason_code !=
            "pending_move_conflict" ||
        mismatched_items.pending_moves_by_id.size() != 1) {
        return fail("mismatched pending move destination was adopted");
    }

    const auto changed_root = temporary.path() / "changed";
    std::filesystem::create_directories(changed_root);
    {
        std::ofstream output{changed_root / "old.txt"};
        output << "data";
    }
    auto changed = file("changed", "folder/new.txt", 4);
    changed.etag = "changed-etag";
    changed.last_modified = "2026-10-04T10:00:00Z";
    changed.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "c6c1c9a9c8543f1e4cd980064cf1625eeb61a90703b2464fff039f21682508b3",
    };
    FakeGraphClient changed_graph;
    changed_graph.changes = {changed};
    changed_graph.contents["changed"] = "next";
    FakeItemStore changed_items;
    changed_items.saved_delta_link = "saved";
    changed_items.items.emplace(
        "changed",
        tracked_item(changed_root, "changed", "old.txt")
    );
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(changed_root / "old.txt") ||
        changed_graph.download_count != 1) {
        return fail("moved file with changed content was not downloaded");
    }
    {
        std::ifstream input{changed_root / "folder" / "new.txt"};
        const std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (content != "next") {
            return fail("moved file did not receive changed remote content");
        }
    }

    const auto directory_root = temporary.path() / "directory";
    std::filesystem::create_directories(directory_root / "Old");
    {
        std::ofstream output{directory_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeGraphClient directory_graph;
    directory_graph.changes = {
        {
            .id = "directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
    };
    FakeItemStore directory_items;
    directory_items.saved_delta_link = "saved";
    directory_items.items.emplace(
        "directory",
        tracked_item(directory_root, "directory", "Old", true)
    );
    directory_items.items.emplace(
        "child",
        tracked_item(directory_root, "child", "Old/child.txt")
    );
    FakeMetrics directory_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_root, false),
            directory_graph,
            directory_items,
            directory_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(directory_root / "Old") ||
        !std::filesystem::exists(
            directory_root / "New" / "child.txt"
        ) ||
        directory_graph.download_count != 0 ||
        directory_items.applied_delta.upserts.size() != 2) {
        return fail("remote directory move did not move its tracked subtree");
    }
    const auto child_state = std::ranges::find(
        directory_items.applied_delta.upserts,
        "child",
        &onedrive::storage::ItemState::remote_id
    );
    if (child_state == directory_items.applied_delta.upserts.end() ||
        child_state->remote_path != "New/child.txt" ||
        child_state->local_path !=
            directory_root / "New" / "child.txt") {
        return fail("remote directory move did not remap descendant state");
    }

    const auto ordered_root = temporary.path() / "ordered-moves";
    std::filesystem::create_directories(ordered_root);
    {
        std::ofstream first{ordered_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient ordered_graph;
    ordered_graph.changes = {
        file("first", "B.txt", 4),
        file("second", "C.txt", 4),
    };
    FakeItemStore ordered_items;
    ordered_items.saved_delta_link = "saved";
    ordered_items.items.emplace(
        "first",
        tracked_item(ordered_root, "first", "A.txt")
    );
    ordered_items.items.emplace(
        "second",
        tracked_item(ordered_root, "second", "B.txt")
    );
    FakeMetrics ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_root, false),
            ordered_graph,
            ordered_items,
            ordered_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(ordered_root / "A.txt") ||
        !std::filesystem::exists(ordered_root / "B.txt") ||
        !std::filesystem::exists(ordered_root / "C.txt") ||
        ordered_graph.download_count != 0 ||
        !ordered_items.applied_delta.blocked_upserts.empty()) {
        return fail("dependent remote move chain was not ordered safely");
    }
    {
        std::ifstream first{ordered_root / "B.txt"};
        std::ifstream second{ordered_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "aaaa" || second_content != "bbbb") {
            return fail("dependent remote move chain swapped local content");
        }
    }

    const auto ordered_recovery_root =
        temporary.path() / "ordered-move-recovery";
    std::filesystem::create_directories(ordered_recovery_root);
    {
        std::ofstream first{ordered_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_recovery_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient failed_ordered_graph;
    failed_ordered_graph.changes = {
        file("recovery-first", "B.txt", 4),
        file("recovery-second", "C.txt", 4),
    };
    FakeItemStore failed_ordered_items;
    failed_ordered_items.saved_delta_link = "saved";
    failed_ordered_items.items.emplace(
        "recovery-first",
        tracked_item(
            ordered_recovery_root,
            "recovery-first",
            "A.txt"
        )
    );
    failed_ordered_items.items.emplace(
        "recovery-second",
        tracked_item(
            ordered_recovery_root,
            "recovery-second",
            "B.txt"
        )
    );
    failed_ordered_items.fail_apply_delta = true;
    FakeMetrics failed_ordered_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            failed_ordered_graph,
            failed_ordered_items,
            failed_ordered_metrics
        }.synchronize());
        return fail("dependent move commit failure did not interrupt sync");
    } catch (const std::runtime_error&) {
    }
    if (failed_ordered_items.pending_moves_by_id.size() != 2) {
        return fail("dependent move failure did not preserve both journals");
    }
    FakeGraphClient recovered_ordered_graph;
    recovered_ordered_graph.changes = failed_ordered_graph.changes;
    FakeItemStore recovered_ordered_items;
    recovered_ordered_items.saved_delta_link = "saved";
    recovered_ordered_items.items = failed_ordered_items.items;
    recovered_ordered_items.pending_moves_by_id =
        failed_ordered_items.pending_moves_by_id;
    FakeMetrics recovered_ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            recovered_ordered_graph,
            recovered_ordered_items,
            recovered_ordered_metrics
        }.synchronize() != 0 ||
        recovered_ordered_graph.download_count != 0 ||
        !recovered_ordered_items.pending_moves_by_id.empty() ||
        !recovered_ordered_metrics.last_success) {
        return fail("dependent remote move journals were not recovered");
    }

    const auto nested_root = temporary.path() / "nested-moves";
    std::filesystem::create_directories(nested_root / "Old");
    {
        std::ofstream output{nested_root / "Old" / "before.txt"};
        output << "data";
    }
    FakeGraphClient nested_graph;
    nested_graph.changes = {
        {
            .id = "nested-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("nested-child", "New/after.txt", 4),
    };
    FakeItemStore nested_items;
    nested_items.saved_delta_link = "saved";
    nested_items.items.emplace(
        "nested-directory",
        tracked_item(
            nested_root,
            "nested-directory",
            "Old",
            true
        )
    );
    nested_items.items.emplace(
        "nested-child",
        tracked_item(
            nested_root,
            "nested-child",
            "Old/before.txt"
        )
    );
    FakeMetrics nested_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nested_root, false),
            nested_graph,
            nested_items,
            nested_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(nested_root / "Old") ||
        std::filesystem::exists(nested_root / "New" / "before.txt") ||
        !std::filesystem::exists(nested_root / "New" / "after.txt") ||
        nested_graph.download_count != 0 ||
        !nested_items.applied_delta.blocked_upserts.empty()) {
        return fail(
            "child rename was not remapped after its parent directory move"
        );
    }

    const auto cycle_root = temporary.path() / "move-cycle";
    std::filesystem::create_directories(cycle_root);
    {
        std::ofstream first{cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{cycle_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient cycle_graph;
    cycle_graph.changes = {
        file("cycle-first", "B.txt", 4),
        file("cycle-second", "A.txt", 4),
    };
    FakeItemStore cycle_items;
    cycle_items.saved_delta_link = "saved";
    cycle_items.items.emplace(
        "cycle-first",
        tracked_item(cycle_root, "cycle-first", "A.txt")
    );
    cycle_items.items.emplace(
        "cycle-second",
        tracked_item(cycle_root, "cycle-second", "B.txt")
    );
    FakeMetrics cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cycle_root, false),
            cycle_graph,
            cycle_items,
            cycle_metrics
        }.synchronize() != 0 ||
        !std::filesystem::exists(cycle_root / "A.txt") ||
        !std::filesystem::exists(cycle_root / "B.txt") ||
        !cycle_items.applied_delta.blocked_upserts.empty() ||
        !cycle_items.pending_moves_by_id.empty()) {
        return fail("remote move dependency cycle was not staged safely");
    }
    {
        std::ifstream first{cycle_root / "A.txt"};
        std::ifstream second{cycle_root / "B.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "bbbb" || second_content != "aaaa") {
            return fail("staged remote name exchange lost local content");
        }
    }
    if (std::ranges::any_of(
            std::filesystem::directory_iterator{cycle_root},
            [](const std::filesystem::directory_entry& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-move-"
                );
            }
        )) {
        return fail("successful name exchange retained its staging path");
    }

    const auto three_cycle_root = temporary.path() / "three-move-cycle";
    std::filesystem::create_directories(three_cycle_root);
    {
        std::ofstream first{three_cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{three_cycle_root / "B.txt"};
        second << "bbbb";
        std::ofstream third{three_cycle_root / "C.txt"};
        third << "cccc";
    }
    FakeGraphClient three_cycle_graph;
    three_cycle_graph.changes = {
        file("three-first", "B.txt", 4),
        file("three-second", "C.txt", 4),
        file("three-third", "A.txt", 4),
    };
    FakeItemStore three_cycle_items;
    three_cycle_items.saved_delta_link = "saved";
    three_cycle_items.items.emplace(
        "three-first",
        tracked_item(
            three_cycle_root,
            "three-first",
            "A.txt"
        )
    );
    three_cycle_items.items.emplace(
        "three-second",
        tracked_item(
            three_cycle_root,
            "three-second",
            "B.txt"
        )
    );
    three_cycle_items.items.emplace(
        "three-third",
        tracked_item(
            three_cycle_root,
            "three-third",
            "C.txt"
        )
    );
    FakeMetrics three_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(three_cycle_root, false),
            three_cycle_graph,
            three_cycle_items,
            three_cycle_metrics
        }.synchronize() != 0 ||
        !three_cycle_items.pending_moves_by_id.empty() ||
        !three_cycle_items.applied_delta.blocked_upserts.empty()) {
        return fail("three-item remote move cycle was not staged safely");
    }
    {
        std::ifstream first{three_cycle_root / "A.txt"};
        std::ifstream second{three_cycle_root / "B.txt"};
        std::ifstream third{three_cycle_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        const std::string third_content{
            std::istreambuf_iterator<char>{third},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "cccc" || second_content != "aaaa" ||
            third_content != "bbbb") {
            return fail("three-item staged move cycle lost local content");
        }
    }

    const auto directory_cycle_root =
        temporary.path() / "directory-move-cycle";
    std::filesystem::create_directories(directory_cycle_root / "A");
    std::filesystem::create_directories(directory_cycle_root / "B");
    {
        std::ofstream first{directory_cycle_root / "A" / "first.txt"};
        first << "aaaa";
        std::ofstream second{directory_cycle_root / "B" / "second.txt"};
        second << "bbbb";
    }
    FakeGraphClient directory_cycle_graph;
    directory_cycle_graph.changes = {
        {
            .id = "cycle-directory-first",
            .name = "B",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "B",
            .directory = true,
        },
        {
            .id = "cycle-directory-second",
            .name = "A",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "A",
            .directory = true,
        },
    };
    FakeItemStore directory_cycle_items;
    directory_cycle_items.saved_delta_link = "saved";
    directory_cycle_items.items.emplace(
        "cycle-directory-first",
        tracked_item(
            directory_cycle_root,
            "cycle-directory-first",
            "A",
            true
        )
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second",
        tracked_item(
            directory_cycle_root,
            "cycle-directory-second",
            "B",
            true
        )
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-first-child",
        tracked_item(
            directory_cycle_root,
            "cycle-directory-first-child",
            "A/first.txt"
        )
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second-child",
        tracked_item(
            directory_cycle_root,
            "cycle-directory-second-child",
            "B/second.txt"
        )
    );
    FakeMetrics directory_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_cycle_root, false),
            directory_cycle_graph,
            directory_cycle_items,
            directory_cycle_metrics
        }.synchronize() != 0 ||
        !std::filesystem::exists(
            directory_cycle_root / "A" / "second.txt"
        ) ||
        !std::filesystem::exists(
            directory_cycle_root / "B" / "first.txt"
        ) ||
        !directory_cycle_items.pending_moves_by_id.empty() ||
        directory_cycle_items.applied_delta.upserts.size() != 4) {
        return fail("remote directory name exchange was not staged safely");
    }

    const auto journaled_staging_root =
        temporary.path() / "journaled-staging-move-cycle";
    std::filesystem::create_directories(journaled_staging_root);
    {
        std::ofstream first{journaled_staging_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{journaled_staging_root / "B.txt"};
        second << "bbbb";
    }
    const auto journaled_staging_path =
        journaled_staging_root / ".A.txt.onedrive-move-journaled";
    struct stat journaled_source_identity{};
    if (::stat(
            (journaled_staging_root / "A.txt").c_str(),
            &journaled_source_identity
        ) == -1) {
        return fail("cannot inspect journaled staging source identity");
    }
    FakeGraphClient journaled_staging_graph;
    journaled_staging_graph.changes = {
        file("journaled-staging-first", "B.txt", 4),
        file("journaled-staging-second", "A.txt", 4),
    };
    FakeItemStore journaled_staging_items;
    journaled_staging_items.saved_delta_link = "saved";
    journaled_staging_items.items.emplace(
        "journaled-staging-first",
        tracked_item(journaled_staging_root, "journaled-staging-first", "A.txt")
    );
    journaled_staging_items.items.emplace(
        "journaled-staging-second",
        tracked_item(
            journaled_staging_root, "journaled-staging-second", "B.txt"
        )
    );
    journaled_staging_items.pending_moves_by_id.emplace(
        "journaled-staging-first",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "journaled-staging-first",
            .source_path = journaled_staging_root / "A.txt",
            .destination_path = journaled_staging_root / "B.txt",
            .staging_path = journaled_staging_path,
            .source_device =
                static_cast<std::uint64_t>(journaled_source_identity.st_dev),
            .source_inode =
                static_cast<std::uint64_t>(journaled_source_identity.st_ino),
        }
    );
    FakeMetrics journaled_staging_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(journaled_staging_root, false),
            journaled_staging_graph,
            journaled_staging_items,
            journaled_staging_metrics
        }
                .synchronize() != 0 ||
        std::filesystem::exists(journaled_staging_path) ||
        !journaled_staging_items.pending_moves_by_id.empty() ||
        !journaled_staging_metrics.last_success) {
        return fail("journaled staging move was not recovered");
    }
    {
        std::ifstream first{journaled_staging_root / "A.txt"};
        std::ifstream second{journaled_staging_root / "B.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "bbbb" || second_content != "aaaa") {
            return fail("journaled staging recovery lost local content");
        }
    }

    const auto staged_recovery_root =
        temporary.path() / "partially-staged-move-cycle";
    std::filesystem::create_directories(staged_recovery_root);
    {
        std::ofstream first{staged_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{staged_recovery_root / "B.txt"};
        second << "bbbb";
    }
    const auto staged_path =
        staged_recovery_root / ".A.txt.onedrive-move-recovery";
    auto staged_first = tracked_item(
        staged_recovery_root,
        "staged-recovery-first",
        "A.txt"
    );
    auto staged_second = tracked_item(
        staged_recovery_root,
        "staged-recovery-second",
        "B.txt"
    );
    std::filesystem::rename(
        staged_recovery_root / "A.txt",
        staged_path
    );
    struct stat staged_identity {};
    if (::stat(staged_path.c_str(), &staged_identity) == -1) {
        return fail("cannot inspect staged recovery fixture identity");
    }
    FakeGraphClient staged_recovery_graph;
    staged_recovery_graph.changes = {
        file("staged-recovery-first", "B.txt", 4),
        file("staged-recovery-second", "A.txt", 4),
    };
    FakeItemStore staged_recovery_items;
    staged_recovery_items.saved_delta_link = "saved";
    staged_recovery_items.items.emplace(
        "staged-recovery-first",
        std::move(staged_first)
    );
    staged_recovery_items.items.emplace(
        "staged-recovery-second",
        std::move(staged_second)
    );
    staged_recovery_items.pending_moves_by_id.emplace(
        "staged-recovery-first",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "staged-recovery-first",
            .source_path = staged_recovery_root / "A.txt",
            .destination_path = staged_recovery_root / "B.txt",
            .staging_path = staged_path,
            .source_device =
                static_cast<std::uint64_t>(staged_identity.st_dev),
            .source_inode =
                static_cast<std::uint64_t>(staged_identity.st_ino),
        }
    );
    FakeMetrics staged_recovery_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(staged_recovery_root, false),
            staged_recovery_graph,
            staged_recovery_items,
            staged_recovery_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(staged_path) ||
        !staged_recovery_items.pending_moves_by_id.empty() ||
        !staged_recovery_metrics.last_success) {
        return fail("partially staged move cycle was not recovered");
    }

    const auto staged_deletion_root =
        temporary.path() / "staged-move-deletion";
    std::filesystem::create_directories(staged_deletion_root);
    const auto staged_deletion_path =
        staged_deletion_root / ".A.txt.onedrive-move-deletion";
    {
        std::ofstream output{staged_deletion_path};
        output << "data";
    }
    struct stat staged_deletion_identity {};
    if (::stat(
            staged_deletion_path.c_str(),
            &staged_deletion_identity
        ) == -1) {
        return fail("cannot inspect staged deletion fixture identity");
    }
    FakeGraphClient staged_deletion_graph;
    staged_deletion_graph.changes = {
        deleted_item("staged-deletion"),
    };
    FakeItemStore staged_deletion_items;
    staged_deletion_items.saved_delta_link = "saved";
    auto staged_deletion_state = tracked_item(
        staged_deletion_root,
        "staged-deletion",
        ".A.txt.onedrive-move-deletion"
    );
    staged_deletion_state.name = "A.txt";
    staged_deletion_state.remote_path = "A.txt";
    staged_deletion_state.local_path = staged_deletion_root / "A.txt";
    staged_deletion_items.items.emplace(
        "staged-deletion",
        std::move(staged_deletion_state)
    );
    staged_deletion_items.pending_moves_by_id.emplace(
        "staged-deletion",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "staged-deletion",
            .source_path = staged_deletion_root / "A.txt",
            .destination_path = staged_deletion_root / "B.txt",
            .staging_path = staged_deletion_path,
            .source_device = static_cast<std::uint64_t>(
                staged_deletion_identity.st_dev
            ),
            .source_inode = static_cast<std::uint64_t>(
                staged_deletion_identity.st_ino
            ),
        }
    );
    FakeMetrics staged_deletion_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(staged_deletion_root, false),
            staged_deletion_graph,
            staged_deletion_items,
            staged_deletion_metrics
        }.synchronize() != 0 ||
        std::filesystem::exists(staged_deletion_path) ||
        !staged_deletion_items.pending_moves_by_id.empty() ||
        staged_deletion_items.applied_delta.removals !=
            std::vector<std::string>{"staged-deletion"}) {
        return fail("remotely deleted staged move was not removed");
    }

    const auto cycle_recovery_root =
        temporary.path() / "move-cycle-recovery";
    std::filesystem::create_directories(cycle_recovery_root);
    {
        std::ofstream first{cycle_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{cycle_recovery_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient failed_cycle_graph;
    failed_cycle_graph.changes = {
        file("recovery-cycle-first", "B.txt", 4),
        file("recovery-cycle-second", "A.txt", 4),
    };
    FakeItemStore failed_cycle_items;
    failed_cycle_items.saved_delta_link = "saved";
    failed_cycle_items.items.emplace(
        "recovery-cycle-first",
        tracked_item(
            cycle_recovery_root,
            "recovery-cycle-first",
            "A.txt"
        )
    );
    failed_cycle_items.items.emplace(
        "recovery-cycle-second",
        tracked_item(
            cycle_recovery_root,
            "recovery-cycle-second",
            "B.txt"
        )
    );
    failed_cycle_items.fail_apply_delta = true;
    FakeMetrics failed_cycle_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(cycle_recovery_root, false),
            failed_cycle_graph,
            failed_cycle_items,
            failed_cycle_metrics
        }.synchronize());
        return fail("staged cycle commit failure did not interrupt sync");
    } catch (const std::runtime_error&) {
    }
    if (failed_cycle_items.pending_moves_by_id.size() != 2) {
        return fail("staged cycle did not retain its recovery journals");
    }
    FakeGraphClient recovered_cycle_graph;
    recovered_cycle_graph.changes = failed_cycle_graph.changes;
    FakeItemStore recovered_cycle_items;
    recovered_cycle_items.saved_delta_link = "saved";
    recovered_cycle_items.items = failed_cycle_items.items;
    recovered_cycle_items.pending_moves_by_id =
        failed_cycle_items.pending_moves_by_id;
    FakeMetrics recovered_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cycle_recovery_root, false),
            recovered_cycle_graph,
            recovered_cycle_items,
            recovered_cycle_metrics
        }.synchronize() != 0 ||
        !recovered_cycle_items.pending_moves_by_id.empty() ||
        recovered_cycle_graph.download_count != 0 ||
        !recovered_cycle_metrics.last_success) {
        return fail("staged move cycle was not recovered after restart");
    }

    const auto blocked_dependency_root =
        temporary.path() / "blocked-move-dependency";
    std::filesystem::create_directories(
        blocked_dependency_root / "Old"
    );
    std::filesystem::create_directories(
        blocked_dependency_root / "New"
    );
    {
        std::ofstream output{
            blocked_dependency_root / "Old" / "before.txt"
        };
        output << "data";
    }
    FakeGraphClient blocked_dependency_graph;
    blocked_dependency_graph.changes = {
        {
            .id = "blocked-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("blocked-child", "New/after.txt", 4),
    };
    FakeItemStore blocked_dependency_items;
    blocked_dependency_items.saved_delta_link = "saved";
    blocked_dependency_items.items.emplace(
        "blocked-directory",
        tracked_item(
            blocked_dependency_root,
            "blocked-directory",
            "Old",
            true
        )
    );
    blocked_dependency_items.items.emplace(
        "blocked-child",
        tracked_item(
            blocked_dependency_root,
            "blocked-child",
            "Old/before.txt"
        )
    );
    FakeMetrics blocked_dependency_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(blocked_dependency_root, false),
            blocked_dependency_graph,
            blocked_dependency_items,
            blocked_dependency_metrics
        }.synchronize() != 2 ||
        !std::filesystem::exists(
            blocked_dependency_root / "Old" / "before.txt"
        ) ||
        std::filesystem::exists(
            blocked_dependency_root / "New" / "after.txt"
        ) ||
        blocked_dependency_items.applied_delta.blocked_upserts.size() != 2 ||
        std::ranges::find(
            blocked_dependency_items.applied_delta.blocked_upserts,
            "move_dependency_blocked",
            &onedrive::storage::BlockedItem::reason_code
        ) ==
            blocked_dependency_items.applied_delta.blocked_upserts.end()) {
        return fail("failed prerequisite did not block its dependent move");
    }

    const auto modified_root = temporary.path() / "modified-move";
    std::filesystem::create_directories(modified_root);
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "data";
    }
    FakeItemStore modified_items;
    modified_items.saved_delta_link = "saved";
    modified_items.items.emplace(
        "modified",
        tracked_item(modified_root, "modified", "old.txt")
    );
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "user data";
    }
    FakeGraphClient modified_graph;
    modified_graph.changes = {file("modified", "new.txt", 4)};
    FakeMetrics modified_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(modified_root, false),
            modified_graph,
            modified_items,
            modified_metrics
        }.synchronize() != 2 ||
        !std::filesystem::exists(modified_root / "old.txt") ||
        std::filesystem::exists(modified_root / "new.txt") ||
        modified_items.applied_delta.blocked_upserts.size() != 1 ||
        modified_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification") {
        return fail("remote move overwrote a locally modified source");
    }

    const auto collision_root = temporary.path() / "collision";
    std::filesystem::create_directories(collision_root);
    {
        std::ofstream source{collision_root / "old.txt"};
        source << "data";
        std::ofstream destination{collision_root / "new.txt"};
        destination << "local";
    }
    FakeItemStore collision_items;
    collision_items.saved_delta_link = "saved";
    collision_items.items.emplace(
        "collision",
        tracked_item(collision_root, "collision", "old.txt")
    );
    FakeGraphClient collision_graph;
    collision_graph.changes = {file("collision", "new.txt", 4)};
    FakeMetrics collision_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(collision_root, false),
            collision_graph,
            collision_items,
            collision_metrics
        }.synchronize() != 2 ||
        !std::filesystem::exists(collision_root / "old.txt") ||
        collision_items.applied_delta.blocked_upserts.size() != 1 ||
        collision_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move replaced an existing local destination");
    }

    const auto dry_root = temporary.path() / "dry-move";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "old.txt"};
        output << "data";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace(
        "dry",
        tracked_item(dry_root, "dry", "old.txt")
    );
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("dry", "new.txt", 4)};
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true),
            dry_graph,
            dry_items,
            dry_metrics
        }.synchronize() != 0 ||
        !std::filesystem::exists(dry_root / "old.txt") ||
        std::filesystem::exists(dry_root / "new.txt") ||
        dry_items.apply_count != 0) {
        return fail("remote move dry run changed local state");
    }

    const auto symlink_root = temporary.path() / "symlink-move";
    const auto outside = temporary.path() / "outside.txt";
    std::filesystem::create_directories(symlink_root);
    {
        std::ofstream output{outside};
        output << "data";
    }
    std::filesystem::create_symlink(outside, symlink_root / "old.txt");
    FakeItemStore symlink_items;
    symlink_items.saved_delta_link = "saved";
    auto symlink_state =
        tracked_item(symlink_root, "symlink", "old.txt");
    symlink_state.local_modified_ticks = 0;
    symlink_items.items.emplace("symlink", std::move(symlink_state));
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {file("symlink", "new.txt", 4)};
    FakeMetrics symlink_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(symlink_root, false),
            symlink_graph,
            symlink_items,
            symlink_metrics
        }.synchronize() != 2 ||
        !std::filesystem::is_symlink(symlink_root / "old.txt") ||
        std::filesystem::exists(symlink_root / "new.txt") ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move followed or replaced a symbolic link");
    }

    const auto shared_memory_root = std::filesystem::path{"/dev/shm"};
    std::error_code device_error;
    if (std::filesystem::is_directory(shared_memory_root, device_error) &&
        !device_error) {
        const auto cross_source_root =
            temporary.path() / "cross-device-source";
        std::filesystem::create_directories(cross_source_root);
        const auto cross_source = cross_source_root / "old.txt";
        {
            std::ofstream output{cross_source};
            output << "data";
        }
        const auto cross_destination_root =
            shared_memory_root /
            ("onedrive-cpp-" + temporary.path().filename().string());
        struct DestinationCleanup {
            std::filesystem::path path;
            ~DestinationCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{cross_destination_root};
        std::filesystem::create_directory(cross_destination_root);
        const auto cross_destination =
            cross_destination_root / "new.txt";
        auto cross_change = file(
            "cross-device",
            cross_destination.lexically_relative("/").generic_string(),
            4
        );
        FakeGraphClient cross_graph;
        cross_graph.changes = {cross_change};
        FakeItemStore cross_items;
        cross_items.saved_delta_link = "saved";
        auto cross_state = tracked_item(
            cross_source_root,
            "cross-device",
            "old.txt"
        );
        cross_state.remote_path =
            cross_source.lexically_relative("/").generic_string();
        cross_items.items.emplace(
            "cross-device",
            std::move(cross_state)
        );
        FakeMetrics cross_metrics;
        auto cross_config = config_for("/", false);
        cross_config.sync_permissions =
            onedrive::config::SyncPermissionsMode::umask;
        if (onedrive::sync::SyncEngine{
                cross_config,
                cross_graph,
                cross_items,
                cross_metrics
            }.synchronize() != 2 ||
            !std::filesystem::exists(cross_source) ||
            std::filesystem::exists(cross_destination) ||
            cross_items.applied_delta.blocked_upserts.size() != 1 ||
            cross_items.applied_delta.blocked_upserts[0].reason_code !=
                "cross_device_move" ||
            !cross_items.pending_moves_by_id.empty() ||
            !cross_metrics.last_success) {
            return fail("cross-device remote move was not blocked safely");
        }
    }
    return EXIT_SUCCESS;
}

int test_pending_download_recovery() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "recovered.txt";
    const auto temporary_file = root / ".recovered.txt.partial";
    {
        std::ofstream output{temporary_file, std::ios::binary};
        output << "data";
    }

    FakeGraphClient graph;
    graph.changes = {
        file("recovered", "recovered.txt", 4),
    };
    FakeItemStore items;
    items.pending.emplace(
        "recovered",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "recovered",
                .name = "recovered.txt",
                .etag = "etag",
                .remote_path = "recovered.txt",
                .local_path = destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = temporary_file,
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    if (onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize() != 0 ||
        !items.pending.empty() || !items.find("me", "recovered") ||
        !std::filesystem::exists(destination) ||
        std::filesystem::exists(temporary_file) || graph.download_count != 0) {
        return fail("pending download was not recovered from the database journal");
    }
    return EXIT_SUCCESS;
}

int test_post_install_recovery() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {file("recover-after-install", "installed.txt", 4)};
    graph.contents["recover-after-install"] = "data";
    FakeItemStore items;
    items.fail_upsert = true;
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize()
        );
        return fail("item persistence failure did not stop synchronization");
    } catch (const std::runtime_error&) {
    }
    if (items.pending.size() != 1 ||
        !std::filesystem::exists(root / "installed.txt") ||
        graph.download_count != 1 || metrics.last_success) {
        return fail("post-install failure did not preserve recovery state");
    }

    items.fail_upsert = false;
    if (onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize() != 0 ||
        !items.pending.empty() ||
        !items.find("me", "recover-after-install") ||
        graph.download_count != 1 || !metrics.last_success) {
        return fail("installed download was not recovered without re-downloading");
    }
    return EXIT_SUCCESS;
}

int test_unsafe_pending_path_rejected() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto outside = temporary.path() / "outside.partial";
    {
        std::ofstream output{outside, std::ios::binary};
        output << "data";
    }
    FakeGraphClient graph;
    FakeItemStore items;
    items.pending.emplace(
        "unsafe",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "unsafe",
                .name = "unsafe.txt",
                .etag = "etag",
                .remote_path = "unsafe.txt",
                .local_path = root / "unsafe.txt",
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = outside,
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{
                config_for(root, false),
                graph,
                items,
                metrics
            }.synchronize()
        );
        return fail("unsafe pending temporary path was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(outside) ||
        std::filesystem::exists(root / "unsafe.txt") ||
        items.pending.size() != 1 || metrics.last_success) {
        return fail("unsafe pending path changed recovery state");
    }
    return EXIT_SUCCESS;
}

int test_mismatched_recovery_file_preserved() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "installed.txt";
    const auto temporary_file = root / ".installed.txt.partial";
    {
        std::ofstream output{destination, std::ios::binary};
        output << "data";
    }
    {
        std::ofstream output{temporary_file, std::ios::binary};
        output << "local";
    }
    FakeGraphClient graph;
    FakeItemStore items;
    items.pending.emplace(
        "installed",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "installed",
                .name = "installed.txt",
                .etag = "etag",
                .remote_path = "installed.txt",
                .local_path = destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = temporary_file,
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{
                config_for(root, false),
                graph,
                items,
                metrics
            }.synchronize()
        );
        return fail("mismatched recovery temporary file was removed");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(destination) ||
        !std::filesystem::exists(temporary_file) ||
        items.pending.size() != 1 || metrics.last_success) {
        return fail("mismatched recovery file was not preserved safely");
    }
    return EXIT_SUCCESS;
}

int test_partial_download_resume() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "resume.txt";
    const auto partial_path =
        root / ".resume.txt.onedrive-partial-previous";
    {
        std::ofstream output{partial_path, std::ios::binary};
        output << "part";
    }

    FakeGraphClient graph;
    graph.changes = {file("resume", "resume.txt", 8)};
    graph.contents["resume"] = "partdata";
    FakeItemStore items;
    items.partials.emplace(
        "resume",
        onedrive::storage::PartialDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "resume",
                .parent_id = "parent",
                .name = "resume.txt",
                .etag = "etag",
                .remote_path = "resume.txt",
                .local_path = destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 8,
            },
            .temporary_path = partial_path,
            .completed_bytes = 4,
        }
    );
    FakeMetrics metrics;
    if (onedrive::sync::SyncEngine{
            config_for(root, false),
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.last_download_offset != 4 ||
        !items.partials.empty() ||
        items.upsert_count != 1 ||
        !metrics.last_success) {
        return fail("partial download did not resume from its saved offset");
    }
    std::ifstream input{destination, std::ios::binary};
    std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (contents != "partdata" || std::filesystem::exists(partial_path)) {
        return fail("resumed download content was not installed correctly");
    }
    return EXIT_SUCCESS;
}

int test_concurrent_failure_cancels_active_download() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("failed", "failed.txt", 4),
        file("interrupted", "interrupted.txt", 4),
    };
    graph.contents["interrupted"] = "data";
    graph.failing_id = "failed";
    graph.cancellable_id = "interrupted";
    graph.downloads_started_before_failure = 2;
    graph.checkpoints_before_failure = 1;
    graph.cancellation_checkpoint = 2;
    FakeItemStore items;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.download_concurrency = 2;

    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{
                config,
                graph,
                items,
                metrics
            }.synchronize()
        );
        return fail("concurrent download failure was accepted");
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()} !=
            "simulated download failure") {
            return fail("download cancellation masked the original failure");
        }
    }

    const auto partial = items.partials.find("interrupted");
    if (partial == items.partials.end() ||
        partial->second.completed_bytes != 2 ||
        !std::filesystem::exists(partial->second.temporary_path) ||
        std::filesystem::file_size(partial->second.temporary_path) != 2 ||
        std::filesystem::exists(root / "interrupted.txt") ||
        items.partials.contains("failed") ||
        items.upsert_count != 0 ||
        metrics.last_success) {
        return fail(
            "cancelled active download did not preserve its durable checkpoint"
        );
    }
    return EXIT_SUCCESS;
}

int test_bounded_concurrent_downloads() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("one", "one.txt", 4),
        file("two", "two.txt", 4),
        file("three", "three.txt", 4),
        file("four", "four.txt", 4),
    };
    for (const auto& item : graph.changes) {
        graph.contents[item.id] = "data";
    }
    graph.download_delay = std::chrono::milliseconds{40};
    FakeItemStore items;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.download_concurrency = 2;

    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.download_count != 4 ||
        graph.maximum_concurrent_downloads != 2 ||
        items.upsert_count != 4 || items.apply_count != 1 ||
        !metrics.last_success) {
        return fail("downloads did not respect the configured concurrency");
    }
    for (const auto& item : graph.changes) {
        if (!std::filesystem::exists(root / item.remote_path)) {
            return fail("a concurrent download was not installed");
        }
    }
    return EXIT_SUCCESS;
}

int test_configured_download_order() {
    const std::vector<onedrive::graph::RemoteItem> changes{
        file("alpha", "alpha.txt", 3),
        file("gamma", "gamma.txt", 1),
        file("beta", "beta.txt", 2),
        file("delta", "delta.txt", 2),
    };
    const std::vector<std::pair<
        onedrive::config::TransferOrder,
        std::vector<std::string>
    >> cases{
        {
            onedrive::config::TransferOrder::default_order,
            {"alpha", "gamma", "beta", "delta"},
        },
        {
            onedrive::config::TransferOrder::size_ascending,
            {"gamma", "beta", "delta", "alpha"},
        },
        {
            onedrive::config::TransferOrder::size_descending,
            {"alpha", "beta", "delta", "gamma"},
        },
        {
            onedrive::config::TransferOrder::name_ascending,
            {"alpha", "beta", "delta", "gamma"},
        },
        {
            onedrive::config::TransferOrder::name_descending,
            {"gamma", "delta", "beta", "alpha"},
        },
    };

    TemporaryDirectory temporary;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto root =
            temporary.path() / ("order-" + std::to_string(index));
        FakeGraphClient graph;
        graph.changes = changes;
        graph.contents = {
            {"alpha", "aaa"},
            {"gamma", "g"},
            {"beta", "bb"},
            {"delta", "dd"},
        };
        std::vector<std::string> started;
        graph.before_download_write =
            [&started](const std::string& remote_id) {
                started.push_back(remote_id);
            };
        FakeItemStore items;
        FakeMetrics metrics;
        auto config = config_for(root, false);
        config.download_concurrency = 1;
        config.transfer_order = cases[index].first;

        if (onedrive::sync::SyncEngine{
                config,
                graph,
                items,
                metrics
            }.synchronize() != 0 ||
            started != cases[index].second ||
            graph.download_count != 4 ||
            items.upsert_count != 4 ||
            !metrics.last_success) {
            return fail("downloads did not follow transfer.order");
        }
    }
    return EXIT_SUCCESS;
}

int test_duplicate_destination_downloads_are_serialized() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("first", "duplicate.txt", 4),
        file("second", "duplicate.txt", 4),
    };
    graph.contents["first"] = "aaaa";
    graph.contents["second"] = "bbbb";
    graph.download_delay = std::chrono::milliseconds{40};
    FakeItemStore items;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.download_concurrency = 2;

    const auto result = onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize();
    std::ifstream input{root / "duplicate.txt", std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (result != 2 || graph.download_count != 1 ||
        graph.maximum_concurrent_downloads != 1 ||
        items.upsert_count != 1 || items.apply_count != 1 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        (contents != "aaaa" && contents != "bbbb") ||
        !metrics.last_success) {
        return fail(
            "duplicate destination downloads were not safely serialized"
        );
    }
    return EXIT_SUCCESS;
}

int test_local_file_uploads() {
    onedrive::test::TemporaryDirectory temporary;
    const auto dry_root = temporary.path() / "dry-uploads";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "new.txt"};
        output << "new";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    FakeGraphClient dry_graph;
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.upload = true;
    if (onedrive::sync::SyncEngine{
            dry_config,
            dry_graph,
            dry_items,
            dry_metrics
        }.synchronize() != 0 ||
        dry_graph.upload_count != 0 || dry_items.apply_count != 0) {
        return fail("upload dry run changed remote or local state");
    }

    const auto root = temporary.path() / "uploads";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "new.txt"};
        output << "new";
    }
    {
        std::ofstream output{root / "modified.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "unchanged.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "tracked-directory"};
        output << "conflict";
    }
    {
        std::ofstream output{root / "report.safeBackup-20261004-0001.txt"};
        output << "backup";
    }
    {
        std::ofstream output{root / ".new.txt.onedrive-move-recovery"};
        output << "staged";
    }
    std::filesystem::create_symlink("new.txt", root / "linked.txt");

    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "modified",
        tracked_item(root, "modified", "modified.txt")
    );
    items.items.emplace(
        "unchanged",
        tracked_item(root, "unchanged", "unchanged.txt")
    );
    items.items.emplace(
        "tracked-directory",
        tracked_item(root, "tracked-directory", "tracked-directory", true)
    );
    {
        std::ofstream output{root / "modified.txt"};
        output << "changed";
    }

    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    const auto result = onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize();
    std::ranges::sort(graph.uploaded_paths);
    bool upload_snapshot_found = false;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().contains(".onedrive-upload-")) {
            upload_snapshot_found = true;
        }
    }
    const auto modified = items.find("me", "modified");
    const auto unchanged = items.find("me", "unchanged");
    if (result != 2 || graph.upload_count != 2 ||
        graph.uploaded_paths !=
            std::vector<std::string>{"modified.txt", "new.txt"} ||
        !modified ||
        (modified->etag != "uploaded-etag-1" &&
         modified->etag != "uploaded-etag-2") ||
        modified->local_size != 7 ||
        modified->local_device == 0 || modified->local_inode == 0 ||
        !unchanged || unchanged->local_device == 0 ||
        unchanged->local_inode == 0 ||
        (!items.find("me", "uploaded-1") &&
         !items.find("me", "uploaded-2")) ||
        upload_snapshot_found || !metrics.last_success) {
        return fail(
            "new and modified local files were not uploaded safely"
        );
    }

    const auto resource_root = temporary.path() / "upload-resources";
    std::filesystem::create_directories(resource_root);
    {
        std::ofstream output{resource_root / "quota.txt"};
        output << "quota";
    }
    {
        std::ofstream output{resource_root / "continued.txt"};
        output << "continued";
    }
    FakeItemStore resource_items;
    resource_items.saved_delta_link = "saved";
    FakeGraphClient resource_graph;
    resource_graph.upload_resource_error_path = "quota.txt";
    FakeMetrics resource_metrics;
    auto resource_config = config_for(resource_root, false);
    resource_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics
    }.synchronize());
    const auto quota_pending =
        resource_items.pending_uploads("me");
    if (quota_pending.size() != 1 ||
        quota_pending[0].remote_path != "quota.txt" ||
        quota_pending[0].failure_code != "remote_quota" ||
        quota_pending[0].failure_attempt_count != 1 ||
        !resource_items.find("me", "uploaded-1")) {
        return fail(
            "remote quota failure was not persisted while uploads continued"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics
    }.synchronize());
    if (resource_items.pending_uploads("me").size() != 1 ||
        resource_items.pending_uploads("me")[0].
            failure_attempt_count != 2) {
        return fail("remote quota retry did not update its failure state");
    }
    resource_graph.upload_resource_error_path.clear();
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics
    }.synchronize());
    if (!resource_items.pending_uploads("me").empty() ||
        !resource_items.find("me", "uploaded-4")) {
        return fail("remote quota upload did not recover");
    }

    const auto storage_root = temporary.path() / "upload-storage";
    std::filesystem::create_directories(storage_root);
    {
        std::ofstream output{storage_root / "blocked.txt"};
        output << "blocked";
    }
    {
        std::ofstream output{storage_root / "continued.txt"};
        output << "continued";
    }
    std::vector<std::filesystem::path> occupied_snapshots;
    for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
        const auto collision =
            storage_root /
            (".blocked.txt.onedrive-upload-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(attempt));
        std::ofstream output{collision};
        output << "occupied";
        occupied_snapshots.push_back(collision);
    }
    FakeItemStore storage_items;
    storage_items.saved_delta_link = "saved";
    FakeGraphClient storage_graph;
    FakeMetrics storage_metrics;
    auto storage_config = config_for(storage_root, false);
    storage_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics
    }.synchronize());
    const auto storage_pending = storage_items.pending_uploads("me");
    if (storage_pending.size() != 1 ||
        storage_pending[0].remote_path != "blocked.txt" ||
        storage_pending[0].failure_code != "local_storage" ||
        storage_pending[0].failure_attempt_count != 1 ||
        storage_graph.uploaded_paths !=
            std::vector<std::string>{"continued.txt"}) {
        return fail(
            "local snapshot failure was not persisted while uploads continued"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics
    }.synchronize());
    const auto retried_storage_pending =
        storage_items.pending_uploads("me");
    if (retried_storage_pending.size() != 1 ||
        retried_storage_pending[0].failure_attempt_count != 2) {
        return fail(
            "local snapshot retry did not update its failure state"
        );
    }
    for (const auto& collision : occupied_snapshots) {
        std::filesystem::remove(collision);
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics
    }.synchronize());
    if (!storage_items.pending_uploads("me").empty() ||
        storage_graph.uploaded_paths !=
            std::vector<std::string>{
                "continued.txt",
                "blocked.txt",
            }) {
        return fail("local snapshot resource failure did not recover");
    }

    if (::geteuid() != 0) {
        const auto permission_root =
            temporary.path() / "upload-permission";
        std::filesystem::create_directories(permission_root);
        const auto unreadable = permission_root / "unreadable.txt";
        {
            std::ofstream output{unreadable};
            output << "unreadable";
        }
        std::filesystem::permissions(
            unreadable,
            std::filesystem::perms::none
        );
        FakeItemStore permission_items;
        permission_items.saved_delta_link = "saved";
        FakeGraphClient permission_graph;
        FakeMetrics permission_metrics;
        auto permission_config = config_for(permission_root, false);
        permission_config.upload = true;
        static_cast<void>(onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics
        }.synchronize());
        const auto permission_pending =
            permission_items.pending_uploads("me");
        if (permission_pending.size() != 1 ||
            permission_pending[0].failure_code != "local_permission") {
            return fail("local upload permission failure was not persisted");
        }
        std::filesystem::permissions(
            unreadable,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );
        static_cast<void>(onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics
        }.synchronize());
        if (!permission_items.pending_uploads("me").empty() ||
            permission_graph.upload_count != 1) {
            return fail("local upload permission failure did not recover");
        }
    }

    const auto removed_root =
        temporary.path() / "removed-upload-resource";
    std::filesystem::create_directories(removed_root);
    FakeItemStore removed_items;
    removed_items.saved_delta_link = "saved";
    removed_items.save_pending_upload({
        .drive_id = "me",
        .remote_path = "removed.txt",
        .local_path = removed_root / "removed.txt",
        .failure_code = "local_read",
        .failure_message = "file was unreadable",
        .failure_attempt_count = 1,
    });
    FakeGraphClient removed_graph;
    FakeMetrics removed_metrics;
    auto removed_config = config_for(removed_root, false);
    removed_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        removed_config,
        removed_graph,
        removed_items,
        removed_metrics
    }.synchronize());
    if (!removed_items.pending_uploads("me").empty()) {
        return fail("removed local upload retained its resource failure");
    }
    return EXIT_SUCCESS;
}

int test_local_deletions() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "deletions";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "retained.txt"};
        output << "retained";
    }
    {
        std::ofstream output{root / "removed.txt"};
        output << "removed";
    }
    std::filesystem::create_directories(root / "Removed");
    {
        std::ofstream output{root / "Removed" / "child.txt"};
        output << "child";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "removed-file",
        tracked_item(root, "removed-file", "removed.txt")
    );
    items.items.emplace(
        "removed-directory",
        tracked_item(root, "removed-directory", "Removed", true)
    );
    items.items.emplace(
        "removed-child",
        tracked_item(root, "removed-child", "Removed/child.txt")
    );
    items.items.emplace(
        "retained",
        tracked_item(root, "retained", "retained.txt")
    );
    std::filesystem::remove(root / "removed.txt");
    std::filesystem::remove_all(root / "Removed");
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize());
    if (graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"removed-directory", "etag"},
                {"removed-file", "etag"},
            } ||
        items.find("me", "removed-directory") ||
        items.find("me", "removed-child") ||
        items.find("me", "removed-file") ||
        !items.find("me", "retained") ||
        !items.pending_deletes_by_id.empty() || !metrics.last_success) {
        return fail(
            "local deletions were not propagated parent-first"
        );
    }

    const auto dry_root = temporary.path() / "dry-deletion";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "removed.txt"};
        output << "removed";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace(
        "dry-removed",
        tracked_item(dry_root, "dry-removed", "removed.txt")
    );
    std::filesystem::remove(dry_root / "removed.txt");
    FakeGraphClient dry_graph;
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        dry_config,
        dry_graph,
        dry_items,
        dry_metrics
    }.synchronize());
    if (!dry_graph.deleted_items.empty() ||
        !dry_items.pending_deletes_by_id.empty() ||
        !dry_items.find("me", "dry-removed")) {
        return fail("deletion dry run changed remote or local state");
    }

    const auto add_missing_tree = [](FakeItemStore& store,
                                     const std::filesystem::path& tree_root) {
        std::filesystem::create_directories(tree_root / "Missing");
        {
            std::ofstream output{tree_root / "Missing" / "a.txt"};
            output << "aaaa";
        }
        {
            std::ofstream output{tree_root / "Missing" / "b.txt"};
            output << "bbbb";
        }
        store.saved_delta_link = "saved";
        store.items.emplace(
            "guard-directory",
            tracked_item(tree_root, "guard-directory", "Missing", true)
        );
        store.items.emplace(
            "guard-child-a",
            tracked_item(tree_root, "guard-child-a", "Missing/a.txt")
        );
        store.items.emplace(
            "guard-child-b",
            tracked_item(tree_root, "guard-child-b", "Missing/b.txt")
        );
        std::filesystem::remove_all(tree_root / "Missing");
    };

    const auto guarded_root = temporary.path() / "guarded-deletion";
    std::filesystem::create_directories(guarded_root);
    FakeItemStore guarded_items;
    add_missing_tree(guarded_items, guarded_root);
    FakeGraphClient guarded_graph;
    FakeMetrics guarded_metrics;
    auto guarded_config = config_for(guarded_root, false);
    guarded_config.upload = true;
    guarded_config.maximum_remote_deletions = 2;
    std::ostringstream guarded_output;
    std::ostringstream guarded_error;
    const onedrive::cli::Console guarded_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
            .quiet = false,
        },
        guarded_output,
        guarded_error
    };
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                              .synchronize());
        return fail("large remote deletion was not blocked");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("large-delete safeguard")) {
            return fail("large remote deletion reported the wrong failure");
        }
    }
    if (!guarded_graph.deleted_items.empty() ||
        !guarded_items.pending_deletes_by_id.empty() ||
        guarded_items.items.size() != 3 || guarded_metrics.last_success ||
        !guarded_output.str().contains("\"delete_operations\":\"1\"") ||
        !guarded_output.str().contains("\"affected_items\":\"3\"") ||
        !guarded_error.str().contains("\"event\":\"large_delete_blocked\"")) {
        return fail(
            "large-delete guard did not preserve state and report its plan"
        );
    }

    FakeItemStore guarded_dry_items;
    add_missing_tree(guarded_dry_items, guarded_root);
    FakeGraphClient guarded_dry_graph;
    FakeMetrics guarded_dry_metrics;
    auto guarded_dry_config = config_for(guarded_root, true);
    guarded_dry_config.upload = true;
    guarded_dry_config.maximum_remote_deletions = 2;
    std::ostringstream guarded_dry_output;
    std::ostringstream guarded_dry_error;
    const onedrive::cli::Console guarded_dry_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
            .quiet = false,
        },
        guarded_dry_output,
        guarded_dry_error
    };
    if (onedrive::sync::SyncEngine{
            guarded_dry_config,
            guarded_dry_graph,
            guarded_dry_items,
            guarded_dry_metrics,
            &guarded_dry_console
        }
                .synchronize() != 0 ||
        !guarded_dry_graph.deleted_items.empty() ||
        guarded_dry_items.items.size() != 3 ||
        !guarded_dry_output.str().contains(
            "\"event\":\"large_delete_detected\""
        ) ||
        !guarded_dry_output.str().contains(
            "\"large_delete_blocked\":\"true\""
        )) {
        return fail(
            "large-delete dry run did not report without changing state"
        );
    }

    guarded_config.force_large_delete = true;
    guarded_output.str({});
    guarded_error.str({});
    if (onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                .synchronize() != 0 ||
        guarded_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"guard-directory", "etag"},
            } ||
        !guarded_items.items.empty() || !guarded_metrics.last_success ||
        !guarded_output.str().contains("\"event\":\"large_delete_forced\"")) {
        return fail("explicit large-delete override did not execute the plan");
    }

    const auto pending_guard_root =
        temporary.path() / "pending-guarded-deletion";
    std::filesystem::create_directories(pending_guard_root);
    FakeItemStore pending_guard_items;
    add_missing_tree(pending_guard_items, pending_guard_root);
    pending_guard_items.pending_deletes_by_id.emplace(
        "guard-directory",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "guard-directory",
            .expected_etag = "etag",
            .remote_path = "Missing",
            .local_path = pending_guard_root / "Missing",
            .directory = true,
        }
    );
    FakeGraphClient pending_guard_graph;
    FakeMetrics pending_guard_metrics;
    auto pending_guard_config = config_for(pending_guard_root, false);
    pending_guard_config.upload = true;
    pending_guard_config.maximum_remote_deletions = 2;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            pending_guard_config,
            pending_guard_graph,
            pending_guard_items,
            pending_guard_metrics
        }
                              .synchronize());
        return fail("pending deletions bypassed the large-delete guard");
    } catch (const std::runtime_error&) {
    }
    if (!pending_guard_graph.deleted_items.empty() ||
        pending_guard_items.pending_deletes_by_id.size() != 1 ||
        pending_guard_items.items.size() != 3 ||
        pending_guard_metrics.last_success) {
        return fail("blocked pending deletions changed remote or local state");
    }

    const auto selective_root = temporary.path() / "selective-deletion";
    std::filesystem::create_directories(selective_root / "Included");
    std::filesystem::create_directories(selective_root / "Excluded");
    {
        std::ofstream output{selective_root / "Included" / "removed.txt"};
        output << "included";
    }
    {
        std::ofstream output{selective_root / "Excluded" / "retained.txt"};
        output << "excluded";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    selective_items.items.emplace(
        "included-delete",
        tracked_item(
            selective_root,
            "included-delete",
            "Included/removed.txt"
        )
    );
    selective_items.items.emplace(
        "excluded-delete",
        tracked_item(
            selective_root,
            "excluded-delete",
            "Excluded/retained.txt"
        )
    );
    std::filesystem::remove(
        selective_root / "Included" / "removed.txt"
    );
    std::filesystem::remove(
        selective_root / "Excluded" / "retained.txt"
    );
    const auto sync_list = temporary.path() / "delete-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeGraphClient selective_graph;
    FakeMetrics selective_metrics;
    auto selective_config = config_for(selective_root, false);
    selective_config.upload = true;
    selective_config.sync_list = sync_list;
    selective_items.saved_sync_filter_fingerprint =
        onedrive::sync::detail::SyncList::load(
            sync_list,
            selective_config.sync_root_files
        ).fingerprint();
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config,
        selective_graph,
        selective_items,
        selective_metrics
    }.synchronize());
    if (selective_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"included-delete", "etag"},
            } ||
        selective_items.find("me", "included-delete") ||
        !selective_items.find("me", "excluded-delete")) {
        return fail("selective sync deletion boundary was not preserved");
    }

    const auto recovery_root = temporary.path() / "delete-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "recover.txt"};
        output << "recover";
    }
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.items.emplace(
        "recover-delete",
        tracked_item(
            recovery_root,
            "recover-delete",
            "recover.txt"
        )
    );
    std::filesystem::remove(recovery_root / "recover.txt");
    recovery_items.fail_commit_delete = true;
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics
        }.synchronize());
        return fail("deletion commit failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_deletes_by_id.size() != 1 ||
        !recovery_items.find("me", "recover-delete")) {
        return fail("deletion commit failure did not retain its journal");
    }
    recovery_items.fail_commit_delete = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics
    }.synchronize());
    if (recovery_graph.deleted_items.size() != 2 ||
        recovery_items.find("me", "recover-delete") ||
        !recovery_items.pending_deletes_by_id.empty()) {
        return fail("pending deletion was not recovered idempotently");
    }

    const auto reappeared_root = temporary.path() / "delete-reappeared";
    std::filesystem::create_directories(reappeared_root);
    {
        std::ofstream output{reappeared_root / "restored.txt"};
        output << "restored";
    }
    FakeItemStore reappeared_items;
    reappeared_items.saved_delta_link = "saved";
    const auto restored =
        tracked_item(reappeared_root, "restored", "restored.txt");
    reappeared_items.items.emplace("restored", restored);
    reappeared_items.pending_deletes_by_id.emplace(
        "restored",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "restored",
            .expected_etag = restored.etag,
            .remote_path = restored.remote_path,
            .local_path = restored.local_path,
        }
    );
    FakeGraphClient reappeared_graph;
    FakeMetrics reappeared_metrics;
    auto reappeared_config = config_for(reappeared_root, false);
    reappeared_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            reappeared_config,
            reappeared_graph,
            reappeared_items,
            reappeared_metrics
        }.synchronize());
        return fail("reappeared local item was deleted remotely");
    } catch (const std::runtime_error&) {
    }
    if (!reappeared_graph.deleted_items.empty() ||
        !reappeared_items.pending_deletes_by_id.empty() ||
        !reappeared_items.find("me", "restored")) {
        return fail("reappeared local item did not cancel deletion recovery");
    }

    const auto conflict_root = temporary.path() / "delete-conflict";
    std::filesystem::create_directories(conflict_root);
    {
        std::ofstream output{conflict_root / "conflict.txt"};
        output << "conflict";
    }
    FakeItemStore conflict_items;
    conflict_items.saved_delta_link = "saved";
    conflict_items.items.emplace(
        "conflict-delete",
        tracked_item(
            conflict_root,
            "conflict-delete",
            "conflict.txt"
        )
    );
    std::filesystem::remove(conflict_root / "conflict.txt");
    FakeGraphClient conflict_graph;
    conflict_graph.delete_conflict = true;
    FakeMetrics conflict_metrics;
    auto conflict_config = config_for(conflict_root, false);
    conflict_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics
        }.synchronize());
        return fail("remote deletion conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!conflict_items.pending_deletes_by_id.empty() ||
        !conflict_items.find("me", "conflict-delete") ||
        conflict_metrics.last_success) {
        return fail("definite deletion conflict retained recovery state");
    }
    return EXIT_SUCCESS;
}

int test_local_move_uploads() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "local-moves";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "old.txt"};
        output << "data";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "moved-file",
        tracked_item(root, "moved-file", "old.txt")
    );
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize());
    const auto before = items.find("me", "moved-file");
    if (!before || before->local_device == 0 ||
        before->local_inode == 0) {
        return fail("local move baseline identity was not persisted");
    }

    std::filesystem::rename(root / "old.txt", root / "renamed.txt");
    static_cast<void>(onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize());
    const auto moved = items.find("me", "moved-file");
    if (graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"moved-file", "renamed.txt"},
            } ||
        !graph.deleted_items.empty() || graph.upload_count != 0 ||
        !moved || moved->remote_path != "renamed.txt" ||
        moved->local_path != root / "renamed.txt" ||
        moved->local_device != before->local_device ||
        moved->local_inode != before->local_inode ||
        !items.pending_remote_moves_by_id.empty()) {
        return fail("local file rename was not applied as a remote move");
    }

    std::filesystem::rename(
        root / "renamed.txt",
        root / "dry-run.txt"
    );
    auto dry_config = config;
    dry_config.dry_run = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        dry_config,
        graph,
        items,
        metrics
    }.synchronize());
    if (graph.moved_remote_items.size() != 1 ||
        !items.pending_remote_moves_by_id.empty() ||
        items.find("me", "moved-file")->remote_path != "renamed.txt") {
        return fail("local move dry run changed remote or persisted state");
    }

    {
        std::ofstream output{
            root / "dry-run.txt",
            std::ios::app
        };
        output << "-changed";
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        config,
        graph,
        items,
        metrics
    }.synchronize());
    if (graph.moved_remote_items.size() != 2 ||
        graph.moved_remote_items.back() !=
            std::pair<std::string, std::string>{
                "moved-file",
                "dry-run.txt",
            } ||
        graph.upload_count != 1 ||
        graph.uploaded_paths !=
            std::vector<std::string>{"dry-run.txt"}) {
        return fail("moved and modified file was not uploaded after move");
    }

    const auto new_parent_root =
        temporary.path() / "move-new-parent";
    std::filesystem::create_directories(new_parent_root);
    {
        std::ofstream output{new_parent_root / "before.txt"};
        output << "data";
    }
    FakeItemStore new_parent_items;
    new_parent_items.saved_delta_link = "saved";
    new_parent_items.items.emplace(
        "new-parent-file",
        tracked_item(
            new_parent_root,
            "new-parent-file",
            "before.txt"
        )
    );
    FakeGraphClient new_parent_graph;
    FakeMetrics new_parent_metrics;
    auto new_parent_config = config_for(new_parent_root, false);
    new_parent_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics
    }.synchronize());
    std::filesystem::create_directories(
        new_parent_root / "New" / "Nested"
    );
    std::filesystem::rename(
        new_parent_root / "before.txt",
        new_parent_root / "New" / "Nested" / "after.txt"
    );
    {
        std::ofstream output{
            new_parent_root / "New" / "Nested" / "after.txt",
            std::ios::app
        };
        output << "-changed";
    }
    auto new_parent_dry_config = new_parent_config;
    new_parent_dry_config.dry_run = true;
    std::ostringstream new_parent_dry_output;
    std::ostringstream new_parent_dry_error;
    const onedrive::cli::Console new_parent_dry_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
        },
        new_parent_dry_output,
        new_parent_dry_error
    };
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_dry_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics,
        &new_parent_dry_console
    }.synchronize());
    const auto new_parent_before_dry_move =
        new_parent_items.find("me", "new-parent-file");
    if (!new_parent_graph.remote_mutations.empty() ||
        new_parent_graph.upload_count != 0 ||
        !new_parent_items.pending_uploads_by_path.empty() ||
        !new_parent_items.pending_remote_moves_by_id.empty() ||
        !new_parent_before_dry_move ||
        new_parent_before_dry_move->remote_path != "before.txt" ||
        !new_parent_dry_output.str().contains(
            R"("create_directories":"2")"
        ) ||
        !new_parent_dry_output.str().contains(
            R"("move_remote_items":"1")"
        ) ||
        !new_parent_dry_error.str().empty()) {
        return fail(
            "new-parent move dry run changed state or reported the wrong plan"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics
    }.synchronize());
    const auto new_parent_file =
        new_parent_items.find("me", "new-parent-file");
    if (new_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "mkdir:New",
                "mkdir:New/Nested",
                "move:new-parent-file:New/Nested/after.txt",
            } ||
        new_parent_graph.uploaded_paths !=
            std::vector<std::string>{"New/Nested/after.txt"} ||
        !new_parent_file ||
        new_parent_file->remote_path != "New/Nested/after.txt" ||
        new_parent_file->local_path !=
            new_parent_root / "New" / "Nested" / "after.txt") {
        return fail(
            "file move into new parent was not ordered before upload"
        );
    }

    const auto tracked_parent_root =
        temporary.path() / "move-tracked-parent";
    std::filesystem::create_directories(
        tracked_parent_root / "Existing"
    );
    {
        std::ofstream output{tracked_parent_root / "before.txt"};
        output << "data";
    }
    FakeItemStore tracked_parent_items;
    tracked_parent_items.saved_delta_link = "saved";
    tracked_parent_items.items.emplace(
        "existing-parent",
        tracked_item(
            tracked_parent_root,
            "existing-parent",
            "Existing",
            true
        )
    );
    tracked_parent_items.items.emplace(
        "tracked-parent-file",
        tracked_item(
            tracked_parent_root,
            "tracked-parent-file",
            "before.txt"
        )
    );
    FakeGraphClient tracked_parent_graph;
    FakeMetrics tracked_parent_metrics;
    auto tracked_parent_config =
        config_for(tracked_parent_root, false);
    tracked_parent_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        tracked_parent_config,
        tracked_parent_graph,
        tracked_parent_items,
        tracked_parent_metrics
    }.synchronize());
    std::filesystem::rename(
        tracked_parent_root / "before.txt",
        tracked_parent_root / "Existing" / "after.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        tracked_parent_config,
        tracked_parent_graph,
        tracked_parent_items,
        tracked_parent_metrics
    }.synchronize());
    if (tracked_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "move:tracked-parent-file:Existing/after.txt",
            } ||
        tracked_parent_graph.directory_create_count != 0) {
        return fail(
            "move into tracked parent created a duplicate directory"
        );
    }

    const auto new_directory_parent_root =
        temporary.path() / "directory-move-new-parent";
    std::filesystem::create_directories(
        new_directory_parent_root / "Old"
    );
    {
        std::ofstream output{
            new_directory_parent_root / "Old" / "child.txt"
        };
        output << "data";
    }
    FakeItemStore new_directory_parent_items;
    new_directory_parent_items.saved_delta_link = "saved";
    new_directory_parent_items.items.emplace(
        "new-parent-directory",
        tracked_item(
            new_directory_parent_root,
            "new-parent-directory",
            "Old",
            true
        )
    );
    auto new_parent_child = tracked_item(
        new_directory_parent_root,
        "new-parent-child",
        "Old/child.txt"
    );
    new_parent_child.parent_id = "new-parent-directory";
    new_directory_parent_items.items.emplace(
        "new-parent-child",
        new_parent_child
    );
    FakeGraphClient new_directory_parent_graph;
    new_directory_parent_graph.moved_item_directory = true;
    FakeMetrics new_directory_parent_metrics;
    auto new_directory_parent_config =
        config_for(new_directory_parent_root, false);
    new_directory_parent_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        new_directory_parent_config,
        new_directory_parent_graph,
        new_directory_parent_items,
        new_directory_parent_metrics
    }.synchronize());
    std::filesystem::create_directories(
        new_directory_parent_root / "New" / "Nested"
    );
    std::filesystem::rename(
        new_directory_parent_root / "Old",
        new_directory_parent_root / "New" / "Nested" / "Old"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        new_directory_parent_config,
        new_directory_parent_graph,
        new_directory_parent_items,
        new_directory_parent_metrics
    }.synchronize());
    const auto new_parent_moved_child =
        new_directory_parent_items.find("me", "new-parent-child");
    if (new_directory_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "mkdir:New",
                "mkdir:New/Nested",
                "move:new-parent-directory:New/Nested/Old",
            } ||
        !new_parent_moved_child ||
        new_parent_moved_child->remote_path !=
            "New/Nested/Old/child.txt" ||
        new_parent_moved_child->local_path !=
            new_directory_parent_root /
                "New" / "Nested" / "Old" / "child.txt" ||
        new_directory_parent_graph.upload_count != 0) {
        return fail(
            "directory move into new parent did not remap descendants"
        );
    }

    const auto recovery_root = temporary.path() / "move-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "before.txt"};
        output << "data";
    }
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.items.emplace(
        "recovery-move",
        tracked_item(
            recovery_root,
            "recovery-move",
            "before.txt"
        )
    );
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics
    }.synchronize());
    std::filesystem::rename(
        recovery_root / "before.txt",
        recovery_root / "after.txt"
    );
    recovery_items.fail_commit_remote_move = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics
        }.synchronize());
        return fail("remote move commit failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_remote_moves_by_id.size() != 1) {
        return fail("remote move commit failure lost its journal");
    }
    recovery_items.fail_commit_remote_move = false;
    recovery_graph.move_conflict = true;
    recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "recovery-move",
        .name = "after.txt",
        .etag = "recovered-etag",
        .parent_id = "root",
        .remote_path = "after.txt",
        .last_modified = "2026-10-05T02:00:00Z",
    };
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics
    }.synchronize());
    const auto recovered =
        recovery_items.find("me", "recovery-move");
    if (!recovered || recovered->remote_path != "after.txt" ||
        !recovery_items.pending_remote_moves_by_id.empty()) {
        return fail("pending remote move was not recovered");
    }

    const auto directory_root = temporary.path() / "directory-move";
    std::filesystem::create_directories(directory_root / "Old");
    {
        std::ofstream output{directory_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeItemStore directory_items;
    directory_items.saved_delta_link = "saved";
    directory_items.items.emplace(
        "move-directory",
        tracked_item(
            directory_root,
            "move-directory",
            "Old",
            true
        )
    );
    directory_items.items.emplace(
        "move-directory-child",
        tracked_item(
            directory_root,
            "move-directory-child",
            "Old/child.txt"
        )
    );
    FakeGraphClient directory_graph;
    directory_graph.moved_item_directory = true;
    FakeMetrics directory_metrics;
    auto directory_config = config_for(directory_root, false);
    directory_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        directory_config,
        directory_graph,
        directory_items,
        directory_metrics
    }.synchronize());
    std::filesystem::rename(
        directory_root / "Old",
        directory_root / "New"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        directory_config,
        directory_graph,
        directory_items,
        directory_metrics
    }.synchronize());
    const auto moved_directory =
        directory_items.find("me", "move-directory");
    const auto moved_child =
        directory_items.find("me", "move-directory-child");
    if (directory_graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"move-directory", "New"},
            } ||
        !directory_graph.deleted_items.empty() ||
        !moved_directory || moved_directory->remote_path != "New" ||
        !moved_child || moved_child->remote_path != "New/child.txt" ||
        moved_child->local_path !=
            directory_root / "New" / "child.txt") {
        return fail(
            "local directory rename did not remap remote descendants"
        );
    }

    const auto selective_root = temporary.path() / "selective-move";
    std::filesystem::create_directories(selective_root / "Included");
    std::filesystem::create_directories(selective_root / "Excluded");
    {
        std::ofstream output{
            selective_root / "Included" / "retained.txt"
        };
        output << "data";
    }
    const auto sync_list = temporary.path() / "move-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    selective_items.items.emplace(
        "selective-move",
        tracked_item(
            selective_root,
            "selective-move",
            "Included/retained.txt"
        )
    );
    FakeGraphClient selective_graph;
    FakeMetrics selective_metrics;
    auto selective_config = config_for(selective_root, false);
    selective_config.upload = true;
    selective_config.sync_list = sync_list;
    selective_items.saved_sync_filter_fingerprint =
        onedrive::sync::detail::SyncList::load(
            sync_list,
            selective_config.sync_root_files
        ).fingerprint();
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config,
        selective_graph,
        selective_items,
        selective_metrics
    }.synchronize());
    std::filesystem::rename(
        selective_root / "Included" / "retained.txt",
        selective_root / "Excluded" / "retained.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config,
        selective_graph,
        selective_items,
        selective_metrics
    }.synchronize());
    if (!selective_graph.moved_remote_items.empty() ||
        !selective_graph.deleted_items.empty() ||
        selective_graph.upload_count != 0 ||
        !selective_items.find("me", "selective-move")) {
        return fail("selective sync boundary move changed remote state");
    }

    const auto selective_parent_root =
        temporary.path() / "selective-new-parent-move";
    std::filesystem::create_directories(
        selective_parent_root / "Included"
    );
    {
        std::ofstream output{
            selective_parent_root / "Included" / "before.txt"
        };
        output << "data";
    }
    FakeItemStore selective_parent_items;
    selective_parent_items.saved_delta_link = "saved";
    selective_parent_items.items.emplace(
        "selective-parent-move",
        tracked_item(
            selective_parent_root,
            "selective-parent-move",
            "Included/before.txt"
        )
    );
    FakeGraphClient selective_parent_graph;
    FakeMetrics selective_parent_metrics;
    auto selective_parent_config =
        config_for(selective_parent_root, false);
    selective_parent_config.upload = true;
    selective_parent_config.sync_list = sync_list;
    selective_parent_items.saved_sync_filter_fingerprint =
        onedrive::sync::detail::SyncList::load(
            sync_list,
            selective_parent_config.sync_root_files
        ).fingerprint();
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_parent_config,
        selective_parent_graph,
        selective_parent_items,
        selective_parent_metrics
    }.synchronize());
    selective_parent_graph.remote_mutations.clear();
    std::filesystem::create_directories(
        selective_parent_root / "Included" / "New" / "Nested"
    );
    std::filesystem::rename(
        selective_parent_root / "Included" / "before.txt",
        selective_parent_root /
            "Included" / "New" / "Nested" / "after.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_parent_config,
        selective_parent_graph,
        selective_parent_items,
        selective_parent_metrics
    }.synchronize());
    if (selective_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "mkdir:Included/New",
                "mkdir:Included/New/Nested",
                "move:selective-parent-move:"
                    "Included/New/Nested/after.txt",
            }) {
        return fail(
            "selective sync did not create included move parents"
        );
    }

    const auto parent_recovery_root =
        temporary.path() / "move-new-parent-recovery";
    std::filesystem::create_directories(parent_recovery_root);
    {
        std::ofstream output{parent_recovery_root / "before.txt"};
        output << "data";
    }
    FakeItemStore parent_recovery_items;
    parent_recovery_items.saved_delta_link = "saved";
    parent_recovery_items.items.emplace(
        "parent-recovery-move",
        tracked_item(
            parent_recovery_root,
            "parent-recovery-move",
            "before.txt"
        )
    );
    FakeGraphClient parent_recovery_graph;
    FakeMetrics parent_recovery_metrics;
    auto parent_recovery_config =
        config_for(parent_recovery_root, false);
    parent_recovery_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        parent_recovery_config,
        parent_recovery_graph,
        parent_recovery_items,
        parent_recovery_metrics
    }.synchronize());
    std::filesystem::create_directories(
        parent_recovery_root / "New" / "Nested"
    );
    std::filesystem::rename(
        parent_recovery_root / "before.txt",
        parent_recovery_root / "New" / "Nested" / "after.txt"
    );
    parent_recovery_graph.move_conflict = true;
    parent_recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "conflicting-item",
        .name = "after.txt",
        .etag = "conflicting-etag",
        .parent_id = "directory-2",
        .remote_path = "New/Nested/after.txt",
    };
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            parent_recovery_config,
            parent_recovery_graph,
            parent_recovery_items,
            parent_recovery_metrics
        }.synchronize());
        return fail("new-parent move conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (parent_recovery_graph.created_directory_paths !=
            std::vector<std::string>{"New", "New/Nested"} ||
        parent_recovery_items.pending_uploads_by_path.size() != 0 ||
        !parent_recovery_items.find("me", "directory-1") ||
        !parent_recovery_items.find("me", "directory-2")) {
        return fail(
            "new-parent move conflict lost created parent state"
        );
    }
    parent_recovery_graph.move_conflict = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        parent_recovery_config,
        parent_recovery_graph,
        parent_recovery_items,
        parent_recovery_metrics
    }.synchronize());
    const auto parent_recovered = parent_recovery_items.find(
        "me",
        "parent-recovery-move"
    );
    if (parent_recovery_graph.directory_create_count != 2 ||
        parent_recovery_graph.moved_remote_items.size() != 2 ||
        !parent_recovered ||
        parent_recovered->remote_path != "New/Nested/after.txt" ||
        !parent_recovery_items.pending_remote_moves_by_id.empty()) {
        return fail(
            "new-parent move did not recover without recreating parents"
        );
    }
    return EXIT_SUCCESS;
}

int test_local_directory_uploads() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "directory-uploads";
    std::filesystem::create_directories(root / "Empty");
    std::filesystem::create_directories(root / "Parent" / "Child");
    {
        std::ofstream output{root / "Parent" / "Child" / "file.txt"};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.created_directory_paths !=
            std::vector<std::string>{
                "Empty",
                "Parent",
                "Parent/Child",
            } ||
        graph.uploaded_paths !=
            std::vector<std::string>{"Parent/Child/file.txt"} ||
        graph.upload_count != 1 ||
        graph.directory_create_count != 3 ||
        items.size() != 4 ||
        !items.find("me", "directory-1") ||
        items.find("me", "directory-1")->local_device == 0 ||
        items.find("me", "directory-1")->local_inode == 0 ||
        !items.pending_uploads_by_path.empty() ||
        !metrics.last_success) {
        return fail(
            "nested local directories were not created before their files"
        );
    }

    const auto resource_root =
        temporary.path() / "directory-resource";
    std::filesystem::create_directories(resource_root / "Continued");
    std::filesystem::create_directories(resource_root / "Quota");
    FakeItemStore resource_items;
    resource_items.saved_delta_link = "saved";
    FakeGraphClient resource_graph;
    resource_graph.directory_resource_error_path = "Quota";
    FakeMetrics resource_metrics;
    auto resource_config = config_for(resource_root, false);
    resource_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics
    }.synchronize());
    const auto resource_pending =
        resource_items.pending_uploads("me");
    if (resource_pending.size() != 1 ||
        resource_pending[0].remote_path != "Quota" ||
        resource_pending[0].failure_code != "remote_quota" ||
        resource_pending[0].failure_attempt_count != 1 ||
        !resource_pending[0].directory ||
        !resource_items.find("me", "directory-1")) {
        return fail(
            "directory quota failure was not persisted while uploads continued"
        );
    }
    resource_graph.directory_resource_error_path.clear();
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics
    }.synchronize());
    if (!resource_items.pending_uploads("me").empty() ||
        !resource_items.find("me", "directory-3")) {
        return fail("directory quota failure did not recover");
    }

    const auto conflict_root =
        temporary.path() / "directory-conflict";
    std::filesystem::create_directories(conflict_root / "Existing");
    FakeItemStore conflict_items;
    conflict_items.saved_delta_link = "saved";
    FakeGraphClient conflict_graph;
    conflict_graph.directory_conflict = true;
    FakeMetrics conflict_metrics;
    auto conflict_config = config_for(conflict_root, false);
    conflict_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics
        }.synchronize());
        return fail("remote directory conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!conflict_items.pending_uploads_by_path.empty() ||
        conflict_metrics.last_success) {
        return fail("definite directory conflict retained recovery state");
    }

    const auto recovery_root =
        temporary.path() / "directory-recovery";
    std::filesystem::create_directories(recovery_root / "Recover");
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.fail_commit_upload = true;
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics
        }.synchronize());
        return fail("directory commit failure was not reported");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_uploads_by_path.size() != 1 ||
        !recovery_items.pending_uploads_by_path.begin()->
            second.directory) {
        return fail("directory commit failure did not retain its journal");
    }
    recovery_items.fail_commit_upload = false;
    recovery_graph.directory_conflict = true;
    recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "recovered-directory",
        .name = "Recover",
        .etag = "recovered-etag",
        .parent_id = "root-id",
        .remote_path = "Recover",
        .directory = true,
    };
    if (onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics
        }.synchronize() != 0 ||
        recovery_graph.directory_create_count != 2 ||
        !recovery_items.pending_uploads_by_path.empty() ||
        !recovery_items.find("me", "recovered-directory") ||
        !recovery_metrics.last_success) {
        return fail("pending directory creation was not recovered");
    }

    const auto selective_root =
        temporary.path() / "selective-directory-upload";
    std::filesystem::create_directories(
        selective_root / "Included"
    );
    std::filesystem::create_directories(
        selective_root / "Excluded"
    );
    const auto sync_list = temporary.path() / "directory-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    FakeGraphClient selective_graph;
    FakeMetrics selective_metrics;
    auto selective_config = config_for(selective_root, false);
    selective_config.upload = true;
    selective_config.sync_list = sync_list;
    if (onedrive::sync::SyncEngine{
            selective_config,
            selective_graph,
            selective_items,
            selective_metrics
        }.synchronize() != 0 ||
        selective_graph.created_directory_paths !=
            std::vector<std::string>{"Included"} ||
        !std::filesystem::is_directory(
            selective_root / "Excluded"
        )) {
        return fail("selective sync uploaded an excluded local directory");
    }

    const auto type_root =
        temporary.path() / "directory-type-conflict";
    std::filesystem::create_directories(type_root / "Tracked");
    FakeItemStore type_items;
    type_items.saved_delta_link = "saved";
    type_items.items.emplace(
        "tracked-file",
        tracked_item(type_root, "tracked-file", "Tracked")
    );
    FakeGraphClient type_graph;
    FakeMetrics type_metrics;
    auto type_config = config_for(type_root, false);
    type_config.upload = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        type_config,
        type_graph,
        type_items,
        type_metrics
    }.synchronize());
    if (type_graph.directory_create_count != 0 ||
        type_items.size() != 1) {
        return fail("local directory replaced a tracked remote file");
    }

    const auto changing_root =
        temporary.path() / "changing-directory-upload";
    const auto changing_directory = changing_root / "Changing";
    std::filesystem::create_directories(changing_directory);
    FakeItemStore changing_items;
    changing_items.saved_delta_link = "saved";
    FakeGraphClient changing_graph;
    changing_graph.before_directory_return = [&] {
        std::filesystem::remove(changing_directory);
    };
    FakeMetrics changing_metrics;
    auto changing_config = config_for(changing_root, false);
    changing_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            changing_config,
            changing_graph,
            changing_items,
            changing_metrics
        }.synchronize());
        return fail("removed local directory was committed after creation");
    } catch (const std::runtime_error&) {
    }
    if (changing_items.pending_uploads_by_path.size() != 1) {
        return fail("changed local directory did not retain recovery state");
    }

    const auto recovery_conflict_root =
        temporary.path() / "directory-recovery-conflict";
    const auto recovery_conflict_directory =
        recovery_conflict_root / "Conflict";
    std::filesystem::create_directories(
        recovery_conflict_directory
    );
    FakeItemStore recovery_conflict_items;
    recovery_conflict_items.saved_delta_link = "saved";
    recovery_conflict_items.pending_uploads_by_path.emplace(
        "Conflict",
        onedrive::storage::PendingUpload{
            .drive_id = "me",
            .remote_path = "Conflict",
            .local_path = recovery_conflict_directory,
            .directory = true,
        }
    );
    FakeGraphClient recovery_conflict_graph;
    recovery_conflict_graph.directory_conflict = true;
    recovery_conflict_graph.lookup_item =
        file("remote-file", "Conflict", 0);
    recovery_conflict_graph.changes = {
        recovery_conflict_graph.lookup_item.value(),
    };
    FakeMetrics recovery_conflict_metrics;
    auto recovery_conflict_config =
        config_for(recovery_conflict_root, false);
    recovery_conflict_config.upload = true;
    if (onedrive::sync::SyncEngine{
            recovery_conflict_config,
            recovery_conflict_graph,
            recovery_conflict_items,
            recovery_conflict_metrics
        }.synchronize() != 2 ||
        !recovery_conflict_items.pending_uploads_by_path.empty() ||
        recovery_conflict_items.applied_delta.blocked_upserts.size() != 1 ||
        recovery_conflict_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !recovery_conflict_metrics.last_success) {
        return fail(
            "directory recovery conflict did not defer to remote delta"
        );
    }
    return EXIT_SUCCESS;
}

int test_local_change_during_upload() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "changing-upload";
    std::filesystem::create_directories(root);
    const auto local = root / "changing.txt";
    {
        std::ofstream output{local};
        output << "old";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.before_upload_return = [&] {
        std::ofstream output{local, std::ios::trunc};
        output << "new contents";
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
            }.synchronize() != 0 ||
        graph.upload_count != 1) {
        return fail("initial changing local file upload failed");
    }
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
            }.synchronize() != 0 ||
        graph.upload_count != 2) {
        return fail("local change during upload was marked as synchronized");
    }
    const auto uploaded = items.find("me", "uploaded-1");
    if (!uploaded || uploaded->local_size != 12 ||
        !items.pending_uploads_by_path.empty() ||
        !metrics.last_success) {
        return fail("follow-up upload did not commit the changed local file");
    }
    return EXIT_SUCCESS;
}

int test_pending_upload_recovery() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "pending-upload";
    std::filesystem::create_directories(root);
    const auto local = root / "recover.txt";
    {
        std::ofstream output{local};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.fail_commit_upload = true;
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize());
        return fail("upload commit failure was not reported");
    } catch (const std::runtime_error&) {
    }
    if (items.pending_uploads_by_path.size() != 1 ||
        !std::filesystem::exists(
            items.pending_uploads_by_path.begin()->second.snapshot_path
        ) ||
        metrics.last_success) {
        return fail("failed upload commit did not preserve its journal");
    }

    items.fail_commit_upload = false;
    graph.upload_conflict = true;
    graph.lookup_item = file("recovered-upload", "recover.txt", 7);
    graph.contents.emplace("recovered-upload", "payload");
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.upload_count != 2 || graph.download_count != 1 ||
        !items.pending_uploads_by_path.empty() ||
        !items.find("me", "recovered-upload") ||
        std::ranges::any_of(
            std::filesystem::directory_iterator{root},
            [](const auto& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-upload-"
                );
            }
        ) ||
        !metrics.last_success) {
        return fail(
            "completed pending upload was not verified and recovered"
        );
    }
    return EXIT_SUCCESS;
}

int test_upload_checkpoint_recovery() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "upload-checkpoint";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "large.bin", std::ios::binary};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.upload_checkpoint = onedrive::graph::UploadSession{
        .upload_url = "https://upload.example.test/session?secret=1",
        .expiration = "2099-10-05T09:00:00Z",
        .completed_bytes = 4,
    };
    graph.fail_after_upload_checkpoint = true;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize());
        return fail("interrupted upload session was not reported");
    } catch (const std::runtime_error&) {
    }
    if (items.pending_uploads_by_path.size() != 1 ||
        items.pending_uploads_by_path.begin()->second.completed_bytes != 4 ||
        items.pending_uploads_by_path.begin()->second.upload_url !=
            "https://upload.example.test/session?secret=1") {
        return fail("upload checkpoint was not persisted before interruption");
    }

    graph.fail_after_upload_checkpoint = false;
    if (onedrive::sync::SyncEngine{
            config,
            graph,
            items,
            metrics
        }.synchronize() != 0 ||
        graph.upload_count != 2 ||
        graph.upload_sessions.size() != 2 ||
        !graph.upload_sessions[1] ||
        graph.upload_sessions[1]->completed_bytes != 4 ||
        graph.upload_sessions[1]->upload_url !=
            "https://upload.example.test/session?secret=1" ||
        !items.pending_uploads_by_path.empty() ||
        !metrics.last_success) {
        return fail("persisted upload checkpoint was not resumed");
    }

    const auto failing_root =
        temporary.path() / "upload-checkpoint-failure";
    std::filesystem::create_directories(failing_root);
    {
        std::ofstream output{failing_root / "large.bin", std::ios::binary};
        output << "payload";
    }
    FakeItemStore failing_items;
    failing_items.saved_delta_link = "saved";
    failing_items.fail_upload_checkpoint_save = true;
    FakeGraphClient failing_graph;
    failing_graph.upload_checkpoint = onedrive::graph::UploadSession{
        .upload_url = "https://upload.example.test/uncommitted",
        .expiration = "2099-10-05T09:00:00Z",
        .completed_bytes = 4,
    };
    FakeMetrics failing_metrics;
    auto failing_config = config_for(failing_root, false);
    failing_config.upload = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            failing_config,
            failing_graph,
            failing_items,
            failing_metrics
        }.synchronize());
        return fail("upload checkpoint persistence failure was ignored");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "checkpoint persistence failure"
            )) {
            return fail("upload checkpoint persistence error was replaced");
        }
    }
    if (failing_graph.upload_count != 1 ||
        failing_items.pending_uploads_by_path.size() != 1 ||
        !failing_items.pending_uploads_by_path.begin()->
             second.upload_url.empty()) {
        return fail("failed upload checkpoint was treated as committed");
    }
    return EXIT_SUCCESS;
}

int test_pending_upload_recovery_conflict() {
    onedrive::test::TemporaryDirectory temporary;
    const auto prepare_conflict =
        [](const std::filesystem::path& root, FakeItemStore& items) {
            std::filesystem::create_directories(root);
            const auto local = root / "conflict.txt";
            const auto snapshot =
                root / ".conflict.txt.onedrive-upload-crash";
            {
                std::ofstream output{local};
                output << "payload";
            }
            std::filesystem::copy_file(local, snapshot);
            auto previous =
                tracked_item(root, "remote-conflict", "conflict.txt");
            previous.local_size = 4;
            items.saved_delta_link = "saved";
            items.items.emplace("remote-conflict", previous);
            items.pending_uploads_by_path.emplace(
                "conflict.txt",
                onedrive::storage::PendingUpload{
                    .drive_id = "me",
                    .remote_path = "conflict.txt",
                    .local_path = local,
                    .snapshot_path = snapshot,
                    .content_fingerprint =
                        "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5",
                    .local_size = 7,
                    .local_modified_ticks =
                        previous.local_modified_ticks,
                    .remote_id =
                        std::optional<std::string>{"remote-conflict"},
                    .expected_etag = previous.etag,
                }
            );
            return snapshot;
        };
    const auto prepare_graph = [](FakeGraphClient& graph) {
        graph.upload_conflict = true;
        graph.lookup_item =
            file("remote-conflict", "conflict.txt", 7);
        graph.changes = {graph.lookup_item.value()};
        graph.contents.emplace("remote-conflict", "changed");
    };

    const auto block_root =
        temporary.path() / "pending-upload-conflict-block";
    FakeItemStore block_items;
    const auto block_snapshot =
        prepare_conflict(block_root, block_items);
    FakeGraphClient block_graph;
    prepare_graph(block_graph);
    FakeMetrics block_metrics;
    auto block_config = config_for(block_root, false);
    block_config.upload = true;
    if (onedrive::sync::SyncEngine{
            block_config,
            block_graph,
            block_items,
            block_metrics
        }.synchronize() != 2 ||
        !block_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(block_snapshot) ||
        block_items.applied_delta.blocked_upserts.size() != 1 ||
        block_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        block_graph.download_count != 1 ||
        !block_metrics.last_success) {
        return fail(
            "upload recovery conflict did not defer to block policy"
        );
    }
    {
        std::ifstream input{block_root / "conflict.txt"};
        std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (content != "payload") {
            return fail("block policy replaced conflicting local content");
        }
    }

    const auto backup_root =
        temporary.path() / "pending-upload-conflict-backup";
    FakeItemStore backup_items;
    const auto backup_snapshot =
        prepare_conflict(backup_root, backup_items);
    FakeGraphClient backup_graph;
    prepare_graph(backup_graph);
    FakeMetrics backup_metrics;
    auto backup_config = config_for(backup_root, false);
    backup_config.upload = true;
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            backup_config,
            backup_graph,
            backup_items,
            backup_metrics
        }.synchronize() != 0 ||
        !backup_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(backup_snapshot) ||
        backup_graph.download_count != 2 ||
        !backup_items.applied_delta.blocked_upserts.empty() ||
        !backup_metrics.last_success) {
        return fail(
            "upload recovery conflict did not defer to backup policy"
        );
    }
    std::filesystem::path preserved;
    for (const auto& entry :
         std::filesystem::directory_iterator{backup_root}) {
        if (entry.path().filename().string().starts_with(
                "conflict.safeBackup-"
            )) {
            preserved = entry.path();
        }
    }
    std::ifstream remote_input{backup_root / "conflict.txt"};
    const std::string remote_content{
        std::istreambuf_iterator<char>{remote_input},
        std::istreambuf_iterator<char>{}
    };
    std::ifstream local_input{preserved};
    const std::string local_content{
        std::istreambuf_iterator<char>{local_input},
        std::istreambuf_iterator<char>{}
    };
    if (preserved.empty() || remote_content != "changed" ||
        local_content != "payload") {
        return fail(
            "backup policy did not preserve both conflict versions"
        );
    }

    const auto size_root =
        temporary.path() / "pending-upload-conflict-size";
    FakeItemStore size_items;
    const auto size_snapshot =
        prepare_conflict(size_root, size_items);
    FakeGraphClient size_graph;
    size_graph.upload_conflict = true;
    size_graph.lookup_item =
        file("remote-conflict", "conflict.txt", 8);
    size_graph.changes = {size_graph.lookup_item.value()};
    size_graph.contents.emplace("remote-conflict", "remote!!");
    FakeMetrics size_metrics;
    auto size_config = config_for(size_root, false);
    size_config.upload = true;
    if (onedrive::sync::SyncEngine{
            size_config,
            size_graph,
            size_items,
            size_metrics
        }.synchronize() != 2 ||
        !size_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(size_snapshot) ||
        size_graph.download_count != 0 ||
        size_items.applied_delta.blocked_upserts.size() != 1 ||
        !size_metrics.last_success) {
        return fail(
            "different-size upload conflict was not reconciled safely"
        );
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_remote_content_tag_strategy();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_refreshes_delta_state();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_remote_moves();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_malware_file_is_blocked_without_overwriting_local_data();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_failure_and_conflict(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_blocked_items_continue_and_retry();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_delta_cursor_restarts_full_query();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_remote_deletions();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_remote_moves();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_download_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_post_install_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_unsafe_pending_path_rejected();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_bounded_concurrent_downloads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_configured_download_order();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_duplicate_destination_downloads_are_serialized();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_file_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_deletions();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_move_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_directory_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_change_during_upload();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_checkpoint_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_partial_download_resume();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_concurrent_failure_cancels_active_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_mismatched_recovery_file_preserved();
}
