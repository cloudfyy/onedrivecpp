#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "support/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onedrive::test::sync {

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

    [[nodiscard]] onedrive::account::DriveIdentity
    drive_identity(std::stop_token = {}) const {
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
    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& path,
        std::stop_token stop_token,
        std::span<const onedrive::graph::RemoteItem> = {}
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated path lookup cancellation"
            };
        }
        return item_by_path(path);
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const onedrive::graph::DeltaProgress& progress,
        std::stop_token,
        std::span<const onedrive::graph::RemoteItem> = {}
    ) const {
        delta_requests.push_back(delta_link);
        if (delta_link && reject_saved_cursor) {
            throw onedrive::graph::DeltaCursorInvalidError{
                "simulated invalid delta cursor"
            };
        }
        auto result =
            delta_override
                ? delta_override(delta_link)
                : onedrive::graph::DeltaResult{
                      .changes = changes,
                      .delta_link = "https://graph.example.test/delta",
                  };
        if (progress) {
            progress(
                1,
                result.changes.size(),
                onedrive::util::ProgressState::completed
            );
        }
        if (stop_on_delta != nullptr) {
            stop_on_delta->request_stop();
        }
        return result;
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
        std::stop_token stop_token,
        const onedrive::graph::UploadProgress& progress = {}
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
        if (during_upload) {
            std::function<void()> callback;
            {
                const std::scoped_lock lock{upload_mutex};
                callback = std::move(during_upload);
                during_upload = {};
            }
            callback();
        }
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated upload cancellation"
            };
        }
        if (upload_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated completed upload"
            };
        }
        if (remote_path == upload_resource_error_path) {
            throw onedrive::graph::UploadResourceError{
                upload_resource_error_code,
                "simulated OneDrive quota exhaustion"
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
        if (progress) {
            const auto size = std::filesystem::file_size(source);
            const auto resumed = session ? session->completed_bytes : 0;
            progress(resumed, size, 0);
            progress(size, size, size - std::min(size, resumed));
        }
        return {
            .id =
                remote_id.value_or("uploaded-" + std::to_string(upload_number)),
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "uploaded-etag-" + std::to_string(upload_number),
            .parent_id = "root-id",
            .remote_path = remote_path,
            .last_modified = "2026-10-04T09:00:00Z",
            .size = upload_enriched_size.value_or(
                static_cast<std::int64_t>(std::filesystem::file_size(source))
            ),
            .content_hash = upload_enriched_hash,
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
    [[nodiscard]] onedrive::graph::RemoteItem create_directory(
        const std::string& remote_path, std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated directory creation cancellation"
            };
        }
        return create_directory(remote_path);
    }

    void delete_item(
        const std::string& remote_id, const std::string& expected_etag
    ) const {
        deleted_items.emplace_back(remote_id, expected_etag);
        if (remote_id == delete_permission_denied_id) {
            throw onedrive::graph::UploadResourceError{
                "remote_permission",
                "simulated remote deletion permission failure"
            };
        }
        if (delete_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated deletion conflict"
            };
        }
    }
    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item deletion cancellation"
            };
        }
        if (during_remote_delete) {
            auto callback = std::move(during_remote_delete);
            during_remote_delete = {};
            callback();
        }
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item deletion cancellation"
            };
        }
        delete_item(remote_id, expected_etag);
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
        if (remote_id == move_permission_denied_id) {
            throw onedrive::graph::UploadResourceError{
                "remote_permission",
                "simulated remote move permission failure"
            };
        }
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
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        const std::string& destination_path,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item move cancellation"
            };
        }
        if (during_remote_move) {
            auto callback = std::move(during_remote_move);
            during_remote_move = {};
            callback();
        }
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item move cancellation"
            };
        }
        return move_item(remote_id, expected_etag, destination_path);
    }

    std::vector<onedrive::graph::RemoteItem> changes;
    std::function<
        onedrive::graph::DeltaResult(const std::optional<std::string>&)>
        delta_override;
    std::optional<onedrive::graph::RemoteItem> lookup_item;
    std::unordered_map<std::string, std::string> contents;
    std::string failing_id;
    std::string cancellable_id;
    std::function<void(const std::string&)> before_download_write;
    mutable std::function<void()> before_upload_return;
    mutable std::function<void()> during_upload;
    mutable std::function<void()> before_directory_return;
    mutable std::function<void()> during_remote_delete;
    mutable std::function<void()> during_remote_move;
    mutable std::optional<onedrive::graph::UploadSession> upload_checkpoint;
    bool reject_saved_cursor{false};
    bool upload_conflict{false};
    bool fatal_upload_error{false};
    std::string upload_resource_error_path;
    bool directory_conflict{false};
    std::string upload_resource_error_code{"remote_quota"};
    std::optional<std::int64_t> upload_enriched_size;
    std::optional<onedrive::util::FileHash> upload_enriched_hash;
    std::string directory_resource_error_path;
    bool delete_conflict{false};
    std::string delete_permission_denied_id;
    bool move_conflict{false};
    std::string move_permission_denied_id;
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
    std::stop_source* stop_on_delta{nullptr};
};

} // namespace onedrive::test::sync
