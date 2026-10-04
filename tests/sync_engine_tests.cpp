#include "onedrive/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/sync_engine.hpp"
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

namespace {

using onedrive::test::TemporaryDirectory;

class FakeGraphClient final {
public:
    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const {
        return {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = "me",
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        };
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
        const std::filesystem::path& source
    ) const {
        ++upload_count;
        uploaded_paths.push_back(remote_path);
        if (upload_conflict) {
            throw onedrive::graph::UploadConflictError{
                "simulated completed upload"
            };
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

    std::vector<onedrive::graph::RemoteItem> changes;
    std::optional<onedrive::graph::RemoteItem> lookup_item;
    std::unordered_map<std::string, std::string> contents;
    std::string failing_id;
    std::string cancellable_id;
    std::function<void(const std::string&)> before_download_write;
    bool reject_saved_cursor{false};
    bool upload_conflict{false};
    std::chrono::milliseconds download_delay{0};
    int downloads_started_before_failure{0};
    int checkpoints_before_failure{0};
    std::size_t cancellation_checkpoint{0};
    mutable std::atomic_int download_count{0};
    mutable int upload_count{0};
    mutable std::vector<std::string> uploaded_paths;
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
        pending_uploads_by_path.insert_or_assign(
            upload.remote_path,
            std::move(upload)
        );
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
            .content_hash = onedrive::FileHash{
                .algorithm = onedrive::FileHashAlgorithm::sha256,
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
    changed.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::sha256,
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
    if (result != 2 || graph.upload_count != 2 ||
        graph.uploaded_paths !=
            std::vector<std::string>{"modified.txt", "new.txt"} ||
        !modified ||
        (modified->etag != "uploaded-etag-1" &&
         modified->etag != "uploaded-etag-2") ||
        modified->local_size != 7 ||
        (!items.find("me", "uploaded-1") &&
         !items.find("me", "uploaded-2")) ||
        upload_snapshot_found || !metrics.last_success) {
        return fail(
            "new and modified local files were not uploaded safely"
        );
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

int test_pending_upload_recovery_conflict() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "pending-upload-conflict";
    std::filesystem::create_directories(root);
    const auto local = root / "conflict.txt";
    const auto snapshot = root / ".conflict.txt.onedrive-upload-crash";
    {
        std::ofstream output{local};
        output << "payload";
    }
    std::filesystem::copy_file(local, snapshot);
    const auto baseline = tracked_item(root, "remote-conflict", "conflict.txt");
    FakeItemStore items;
    items.saved_delta_link = "saved";
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
            .local_modified_ticks = baseline.local_modified_ticks,
        }
    );
    FakeGraphClient graph;
    graph.upload_conflict = true;
    graph.lookup_item = file("remote-conflict", "conflict.txt", 7);
    graph.contents.emplace("remote-conflict", "changed");
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
        return fail("conflicting pending upload recovery was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "remote upload recovery content conflicts"
            )) {
            throw;
        }
    }
    if (items.pending_uploads_by_path.size() != 1 ||
        !std::filesystem::exists(snapshot) || metrics.last_success) {
        return fail("conflicting pending upload journal was not preserved");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
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
    if (const int result = test_pending_upload_recovery();
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
