#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/upload/remote_delete.hpp"

#include <cerrno>
#include <cstring>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace onedrive::test::sync;
using namespace std::chrono_literals;

std::atomic_bool remove_during_identity{false};
std::atomic_int identities_before_removal{0};

int test_delete_during_watch_scan(int identities) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    std::filesystem::create_directory(root);
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    std::atomic_int rounds{0};
    std::atomic_bool deleted{false};
    std::atomic_bool deferred{false};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token& stop) {
            const auto result =
                onedrive::sync::SyncEngine{
                    config, graph, items, metrics, &console
                }
                    .synchronize(stop);
            if (++rounds == 1) {
                identities_before_removal = identities;
                remove_during_identity = true;
                if (::chmod((root / "copied.txt").c_str(), 0600) != 0) {
                    throw std::runtime_error{"cannot trigger local scan"};
                }
            } else {
                deferred = deferred || result == onedrive::exit_code::failure;
                deleted = !graph.deleted_items.empty();
            }
            return result;
        },
        1h,
        5ms,
    };
    std::exception_ptr error;
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        try {
            result = monitor.run(stop);
        } catch (...) {
            error = std::current_exception();
        }
    }};
    const bool reconciled =
        onedrive::test::wait_until([&] { return deleted.load(); }, 3s, 5ms);
    worker.request_stop();
    worker.join();
    remove_during_identity = false;
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& failure) {
            return fail(
                "watch exited when an uploaded file disappeared during scan: " +
                std::string{failure.what()}
            );
        }
    }
    if (!reconciled || !deferred || rounds < 3 ||
        result != onedrive::exit_code::success || graph.upload_count != 1 ||
        graph.download_count != 0 || graph.deleted_items.size() != 1 ||
        std::filesystem::exists(root / "copied.txt") || !items.items.empty() ||
        !items.blocked.empty()) {
        return fail(
            "watch did not reconcile deletion after an interrupted scan"
        );
    }
    return EXIT_SUCCESS;
}

int test_delete_during_delta_reuse() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace("file", tracked_item(root, "file", "copied.txt"));
    FakeGraphClient graph;
    graph.delta_override = [&](const auto&) {
        identities_before_removal = 0;
        remove_during_identity = true;
        return onedrive::graph::DeltaResult{
            .changes = {file("file", "copied.txt", 4)},
            .delta_link = "next",
        };
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    const auto result =
        onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
            .synchronize();
    if (remove_during_identity.exchange(false) ||
        result != onedrive::exit_code::success || graph.download_count != 0 ||
        graph.upload_count != 0 || graph.deleted_items.size() != 1 ||
        !items.items.empty() || !items.blocked.empty()) {
        return fail("Delta reuse resurrected a concurrently deleted file");
    }
    return EXIT_SUCCESS;
}

int test_delete_before_upload_commit() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.before_upload_return = [] {
        identities_before_removal = 0;
        remove_during_identity = true;
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    const auto synchronize = [&] {
        return onedrive::sync::SyncEngine{
            config, graph, items, metrics, &console
        }
            .synchronize();
    };
    if (synchronize() != onedrive::exit_code::success ||
        remove_during_identity.exchange(false) || items.items.size() != 1 ||
        !items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(root / "copied.txt") ||
        synchronize() != onedrive::exit_code::success ||
        graph.download_count != 0 || graph.upload_count != 1 ||
        graph.deleted_items.size() != 1 || !items.items.empty() ||
        !items.blocked.empty()) {
        return fail("upload commit lost a concurrently deleted source");
    }
    return EXIT_SUCCESS;
}

int test_delete_metadata_version_race() {
    for (const int scenario : {0, 1, 2, 3, 4}) {
        TemporaryDirectory temporary;
        const auto root = temporary.path();
        std::ofstream{root / "copied.txt"} << "data";
        FakeItemStore items;
        auto previous = tracked_item(root, "file", "copied.txt");
        previous.ctag = "content-v1";
        items.items.emplace("file", previous);
        std::filesystem::remove(root / "copied.txt");
        FakeGraphClient graph;
        auto current = file("file", "copied.txt", 4);
        current.etag = "metadata-v2";
        current.ctag = scenario == 1 ? "content-v2" : "content-v1";
        if (scenario == 2) {
            current.id = "replacement";
        }
        graph.lookup_item = current;
        graph.delete_conflict = true;
        graph.during_remote_delete = [&] {
            if (scenario == 4) {
                std::ofstream{root / "copied.txt"} << "keep";
            }
            graph.during_remote_delete = [&] {
                graph.delete_conflict = scenario == 3;
            };
        };
        onedrive::graph::GraphClient graph_port{
            onedrive::util::borrowed_proxy, graph
        };
        onedrive::storage::ItemStore store{
            onedrive::util::borrowed_proxy, items
        };
        bool conflict = false;
        try {
            onedrive::sync::detail::execute_new_remote_delete(
                {
                    .drive_id = "me",
                    .remote_id = "file",
                    .expected_etag = previous.etag,
                    .remote_path = previous.remote_path,
                    .local_path = previous.local_path,
                },
                graph_port,
                store
            );
        } catch (
            const onedrive::sync::detail::LocalModificationConflictError&
        ) {
            conflict = true;
        }
        const bool retry = scenario == 0 || scenario == 3;
        if (conflict != (scenario != 0) ||
            graph.deleted_items.size() != (retry ? 2U : 1U) ||
            (retry && graph.deleted_items.back().second != "metadata-v2") ||
            items.items.empty() != (scenario == 0) ||
            !items.pending_deletes_by_id.empty() ||
            items.blocked.empty() != (scenario == 0) ||
            (scenario == 4 &&
             onedrive::test::read_file(root / "copied.txt") != "keep")) {
            return fail(
                "metadata-only deletion retry bypassed conflict safety"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_create_delete_during_sync() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    std::filesystem::create_directory(root);
    std::atomic_int rounds{0};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token&) {
            if (++rounds == 1) {
                std::ofstream{root / "copied.txt"} << "data";
                std::filesystem::remove(root / "copied.txt");
            }
            return onedrive::exit_code::success;
        },
        1h,
        5ms,
    };
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        result = monitor.run(stop);
    }};
    const bool observed =
        onedrive::test::wait_until([&] { return rounds >= 2; }, 2s, 5ms);
    worker.request_stop();
    worker.join();
    if (!observed || result != onedrive::exit_code::success) {
        return fail(
            "monitor discarded a user's coalesced create/delete events"
        );
    }
    return EXIT_SUCCESS;
}

int test_transient_probe_is_ignored() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::atomic_int rounds{0};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token&) {
            if (++rounds == 1) {
                const auto probe = root / ".onedrive-cpp-xattr-probe-test";
                std::ofstream{probe} << "probe";
                std::filesystem::remove(probe);
            }
            return onedrive::exit_code::success;
        },
        1h,
        5ms,
    };
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        result = monitor.run(stop);
    }};
    const bool started =
        onedrive::test::wait_until([&] { return rounds >= 1; }, 2s, 5ms);
    std::this_thread::sleep_for(100ms);
    worker.request_stop();
    worker.join();
    if (!started || rounds != 1 || result != onedrive::exit_code::success) {
        return fail("transient internal probes caused a watch feedback loop");
    }
    return EXIT_SUCCESS;
}

int test_failure_log_includes_exception() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "file.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.fatal_upload_error = true;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    std::ostringstream messages;
    const auto previous_logger = spdlog::default_logger();
    const auto sink =
        std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("watch-race-failure", sink)
    );
    bool failed = false;
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
                .synchronize()
        );
    } catch (const std::runtime_error&) {
        failed = true;
    }
    spdlog::set_default_logger(previous_logger);
    if (!failed || !messages.str().contains("Synchronization failed after") ||
        !messages.str().contains("simulated fatal upload failure")) {
        return fail("synchronization log omitted the underlying exception");
    }
    return EXIT_SUCCESS;
}

} // namespace

extern "C" {
int __real_fstatat(int, const char*, struct stat*, int);

int __wrap_fstatat(
    int directory, const char* path, struct stat* status, int flags
) {
    if (std::strcmp(path, "copied.txt") == 0 && remove_during_identity.load() &&
        identities_before_removal.fetch_sub(1) == 0 &&
        remove_during_identity.exchange(false)) {
        if (::unlinkat(directory, path, 0) == 0) {
            errno = ENOENT;
            return -1;
        }
    }
    return __real_fstatat(directory, path, status, flags);
}
}

int main() {
    for (const int identities : {0, 1}) {
        if (const auto result = test_delete_during_watch_scan(identities);
            result != EXIT_SUCCESS) {
            return result;
        }
    }
    if (const auto result = test_delete_during_delta_reuse();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_delete_before_upload_commit();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_delete_metadata_version_race();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_create_delete_during_sync();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_transient_probe_is_ignored();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_failure_log_includes_exception();
}
