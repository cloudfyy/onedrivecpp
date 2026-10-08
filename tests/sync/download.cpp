#include "support.hpp"

namespace {

using namespace onedrive::test::sync;

int test_pending_download_recovery() {
    const onedrive::cli::Console default_console;
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
            .item =
                {
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
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        !items.pending.empty() || !items.find("me", "recovered") ||
        !std::filesystem::exists(destination) ||
        std::filesystem::exists(temporary_file) || graph.download_count != 0) {
        return fail(
            "pending download was not recovered from the database journal"
        );
    }
    return EXIT_SUCCESS;
}

int test_post_install_recovery() {
    const onedrive::cli::Console default_console;
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
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("item persistence failure did not stop synchronization");
    } catch (const std::runtime_error&) {
    }
    if (items.pending.size() != 1 ||
        !std::filesystem::exists(root / "installed.txt") ||
        graph.download_count != 1 || metrics.last_success) {
        return fail("post-install failure did not preserve recovery state");
    }

    items.fail_upsert = false;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        !items.pending.empty() || !items.find("me", "recover-after-install") ||
        graph.download_count != 1 || !metrics.last_success) {
        return fail(
            "installed download was not recovered without re-downloading"
        );
    }
    return EXIT_SUCCESS;
}

int test_unsafe_pending_path_rejected() {
    const onedrive::cli::Console default_console;
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
            .item =
                {
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
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                              .synchronize());
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
    const onedrive::cli::Console default_console;
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
            .item =
                {
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
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
        }
    );
    FakeMetrics metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("mismatched recovery temporary file was removed");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(destination) ||
        !std::filesystem::exists(temporary_file) || items.pending.size() != 1 ||
        metrics.last_success) {
        return fail("mismatched recovery file was not preserved safely");
    }
    return EXIT_SUCCESS;
}

int test_partial_download_resume() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "resume.txt";
    const auto partial_path = root / ".resume.txt.onedrive-partial-previous";
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
            .item =
                {
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
            config_for(root, false), graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.last_download_offset != 4 || !items.partials.empty() ||
        items.upsert_count != 1 || !metrics.last_success) {
        return fail("partial download did not resume from its saved offset");
    }
    std::ifstream input{destination, std::ios::binary};
    std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}
    };
    if (contents != "partdata" || std::filesystem::exists(partial_path)) {
        return fail("resumed download content was not installed correctly");
    }
    return EXIT_SUCCESS;
}

int test_concurrent_failure_cancels_active_download() {
    const onedrive::cli::Console default_console;
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
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("concurrent download failure was accepted");
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()} != "simulated download failure") {
            return fail("download cancellation masked the original failure");
        }
    }

    const auto partial = items.partials.find("interrupted");
    if (partial == items.partials.end() ||
        partial->second.completed_bytes != 2 ||
        !std::filesystem::exists(partial->second.temporary_path) ||
        std::filesystem::file_size(partial->second.temporary_path) != 2 ||
        std::filesystem::exists(root / "interrupted.txt") ||
        items.partials.contains("failed") || items.upsert_count != 0 ||
        metrics.last_success) {
        return fail(
            "cancelled active download did not preserve its durable checkpoint"
        );
    }
    return EXIT_SUCCESS;
}

int test_bounded_concurrent_downloads() {
    const onedrive::cli::Console default_console;
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
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.download_count != 4 || graph.maximum_concurrent_downloads != 2 ||
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
    const onedrive::cli::Console default_console;
    const std::vector<onedrive::graph::RemoteItem> changes{
        file("alpha", "alpha.txt", 3),
        file("gamma", "gamma.txt", 1),
        file("beta", "beta.txt", 2),
        file("delta", "delta.txt", 2),
    };
    const std::vector<
        std::pair<onedrive::config::TransferOrder, std::vector<std::string>>>
        cases{
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
        const auto root = temporary.path() / ("order-" + std::to_string(index));
        FakeGraphClient graph;
        graph.changes = changes;
        graph.contents = {
            {"alpha", "aaa"},
            {"gamma", "g"},
            {"beta", "bb"},
            {"delta", "dd"},
        };
        std::vector<std::string> started;
        graph.before_download_write = [&started](const std::string& remote_id) {
            started.push_back(remote_id);
        };
        FakeItemStore items;
        FakeMetrics metrics;
        auto config = config_for(root, false);
        config.download_concurrency = 1;
        config.transfer_order = cases[index].first;

        if (onedrive::sync::SyncEngine{
                config, graph, items, metrics, &default_console
            }
                    .synchronize() != 0 ||
            started != cases[index].second || graph.download_count != 4 ||
            items.upsert_count != 4 || !metrics.last_success) {
            return fail("downloads did not follow transfer.order");
        }
    }
    return EXIT_SUCCESS;
}

int test_duplicate_destination_downloads_are_serialized() {
    const onedrive::cli::Console default_console;
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

    const auto result =
        onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
            .synchronize();
    std::ifstream input{root / "duplicate.txt", std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}
    };
    if (result != 2 || graph.download_count != 1 ||
        graph.maximum_concurrent_downloads != 1 || items.upsert_count != 1 ||
        items.apply_count != 1 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        (contents != "aaaa" && contents != "bbbb") || !metrics.last_success) {
        return fail(
            "duplicate destination downloads were not safely serialized"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
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
    if (const int result = test_mismatched_recovery_file_preserved();
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
    return EXIT_SUCCESS;
}
