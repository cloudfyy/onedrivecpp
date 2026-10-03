#include "onedrive/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/sync_engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-sync-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class FakeGraphClient final : public onedrive::graph::GraphClient {
public:
    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const override {
        return {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = "me",
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const override {
        return {};
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const onedrive::graph::DeltaProgress& progress
    ) const override {
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
        std::uint64_t,
        const std::filesystem::path& destination,
        const onedrive::graph::DownloadProgress& progress
    ) const override {
        ++download_count;
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
            std::this_thread::sleep_for(download_delay);
        }
        if (remote_id == failing_id) {
            throw std::runtime_error{"simulated download failure"};
        }
        std::ofstream output{destination, std::ios::binary};
        output << contents.at(remote_id);
        if (progress) {
            const auto size = contents.at(remote_id).size();
            progress(size / 2, size);
            progress(size, size);
        }
    }

    std::vector<onedrive::graph::RemoteItem> changes;
    std::unordered_map<std::string, std::string> contents;
    std::string failing_id;
    bool reject_saved_cursor{false};
    std::chrono::milliseconds download_delay{0};
    mutable std::atomic_int download_count{0};
    mutable std::atomic_int active_downloads{0};
    mutable std::atomic_int maximum_concurrent_downloads{0};
    mutable std::vector<std::optional<std::string>> delta_requests;
};

class FakeItemStore final : public onedrive::storage::ItemStore {
public:
    void open() override {}

    void upsert(onedrive::storage::ItemState item) override {
        if (fail_upsert) {
            throw std::runtime_error{"simulated item persistence failure"};
        }
        ++upsert_count;
        items.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta delta) override {
        ++apply_count;
        applied_delta = std::move(delta);
    }

    void save_pending_download(
        onedrive::storage::PendingDownload download
    ) override {
        pending.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_pending_download(
        const std::string&,
        const std::string& remote_id
    ) override {
        pending.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const override {
        std::vector<onedrive::storage::PendingDownload> result;
        for (const auto& [remote_id, download] : pending) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem> blocked_items(
        const std::string&
    ) const override {
        return blocked;
    }

    bool reset(const std::string&) override {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) override {
        return {};
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const override {
        return saved_delta_link;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState> find(
        const std::string&,
        const std::string& remote_id
    ) const override {
        const auto iterator = items.find(remote_id);
        return iterator == items.end() ?
                   std::nullopt :
                   std::optional<onedrive::storage::ItemState>{
                       iterator->second
                   };
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return items.size();
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> items;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::vector<onedrive::storage::BlockedItem> blocked;
    onedrive::storage::ItemDelta applied_delta;
    std::optional<std::string> saved_delta_link;
    int upsert_count{0};
    int apply_count{0};
    bool fail_upsert{false};
};

class FakeMetrics final : public onedrive::metrics::Metrics {
public:
    void record_sync_run(
        bool success,
        std::chrono::duration<double>
    ) noexcept override {
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

}  // namespace

int main() {
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
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
    return test_mismatched_recovery_file_preserved();
}
