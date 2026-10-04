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
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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
        const std::string&
    ) const {
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

    std::vector<onedrive::graph::RemoteItem> changes;
    std::unordered_map<std::string, std::string> contents;
    std::string failing_id;
    std::string cancellable_id;
    std::function<void(const std::string&)> before_download_write;
    bool reject_saved_cursor{false};
    std::chrono::milliseconds download_delay{0};
    int downloads_started_before_failure{0};
    int checkpoints_before_failure{0};
    std::size_t cancellation_checkpoint{0};
    mutable std::atomic_int download_count{0};
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
        ++apply_count;
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

    std::unordered_map<std::string, onedrive::storage::ItemState> items;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::unordered_map<std::string, onedrive::storage::PartialDownload> partials;
    std::vector<onedrive::storage::BlockedItem> blocked;
    onedrive::storage::ItemDelta applied_delta;
    std::optional<std::string> saved_delta_link;
    std::optional<std::string> saved_sync_filter_fingerprint;
    int upsert_count{0};
    int apply_count{0};
    bool fail_upsert{false};
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

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

onedrive::config::Config config_for(
    const std::filesystem::path& root,
    bool dry_run
) {
    auto config = onedrive::config::Config::defaults();
    config.sync_directory = root;
    config.state_directory = root.parent_path() / "state";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
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
        file("included", "Documents/included.txt", 4),
        file("excluded", "Pictures/excluded.txt", 7),
    };
    graph.contents["included"] = "data";
    graph.contents["excluded"] = "ignored";
    FakeItemStore items;
    items.saved_delta_link = "https://graph.example.test/old-delta";
    items.saved_sync_filter_fingerprint = "old-filter";
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
        std::filesystem::exists(root / "Pictures/excluded.txt")) {
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

}  // namespace

int main() {
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_refreshes_delta_state();
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
