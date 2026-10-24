#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/ui/cli/backend.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "base_cases.hpp"

namespace onedrive::test::sync::base_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

namespace {

class StopAtMessageObserver final : public onedrive::events::Observer {
public:
    StopAtMessageObserver(std::stop_source& stop, std::string target_event)
        : stop_{stop},
          target_event_{std::move(target_event)} {
    }

protected:
    void on_event(const onedrive::events::Event& event) const override {
        const auto* message =
            std::get_if<onedrive::events::MessageEvent>(&event);
        if (message && message->event == target_event_) {
            stop_.request_stop();
            triggered = true;
        }
    }

public:
    mutable bool triggered{false};

private:
    std::stop_source& stop_;
    std::string target_event_;
};

} // namespace

int test_engine_owns_configuration() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "owned-config";
    FakeGraphClient graph;
    FakeItemStore items;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    onedrive::sync::SyncEngine engine{
        config, graph, items, metrics, &default_console
    };
    config.sync_data_mount_point = temporary.path() / "missing-mount";
    config.drive_id = "mutated";

    if (engine.synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        items.applied_delta.drive_id != "me") {
        return fail("sync engine did not preserve its owned configuration");
    }
    return EXIT_SUCCESS;
}

int test_sync_cancellation_does_not_commit_delta() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    auto config = config_for(temporary.path() / "cancelled-sync", false);
    FakeGraphClient graph;
    FakeItemStore items;
    FakeMetrics metrics;
    std::stop_source stop;
    graph.stop_on_delta = &stop;
    onedrive::sync::SyncEngine engine{
        config, graph, items, metrics, &default_console
    };

    if (engine.synchronize(stop.get_token()) != exit_code::cancelled ||
        items.apply_count != 0 ||
        metrics.last_outcome != onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail("cancelled synchronization committed its delta state");
    }
    return EXIT_SUCCESS;
}

int test_pre_cancelled_sync_does_not_start() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    auto config = config_for(temporary.path() / "pre-cancelled-sync", false);
    FakeGraphClient graph;
    FakeItemStore items;
    FakeMetrics metrics;
    std::stop_source stop;
    stop.request_stop();
    onedrive::sync::SyncEngine engine{
        config, graph, items, metrics, &default_console
    };

    if (engine.synchronize(stop.get_token()) != exit_code::cancelled ||
        !graph.delta_requests.empty() || items.apply_count != 0 ||
        metrics.last_outcome != onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail("pre-cancelled synchronization performed work");
    }
    return EXIT_SUCCESS;
}

int test_sync_cancellation_at_execution_boundary() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "cancel-before-execution";
    auto config = config_for(root, false);
    FakeGraphClient graph;
    graph.changes = {file("remote-file", "remote.txt", 4)};
    graph.contents["remote-file"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    std::stop_source stop;
    StopAtMessageObserver observer{stop, "execution_started"};
    onedrive::sync::SyncEngine engine{config, graph, items, metrics, &observer};

    if (engine.synchronize(stop.get_token()) != exit_code::cancelled ||
        !observer.triggered || graph.delta_requests.size() != 1 ||
        items.apply_count != 0 ||
        std::filesystem::exists(root / "remote.txt") ||
        metrics.last_outcome != onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail(
            "cancellation at the execution boundary applied the pending delta"
        );
    }
    return EXIT_SUCCESS;
}

int test_dry_run_and_success() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto guarded_root = temporary.path() / "guarded";
    FakeGraphClient guarded_graph;
    FakeItemStore guarded_items;
    FakeMetrics guarded_metrics;
    auto guarded_config = config_for(guarded_root, false);
    guarded_config.sync_data_mount_point = temporary.path();
    std::ostringstream guarded_output;
    std::ostringstream guarded_error;
    const onedrive::cli::Console guarded_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
        },
        guarded_output,
        guarded_error
    };
    if (onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                .synchronize() == exit_code::success ||
        !guarded_graph.delta_requests.empty() ||
        std::filesystem::exists(guarded_root) || guarded_metrics.last_success ||
        !guarded_output.str().empty() ||
        !guarded_error.str().contains("\"event\":\"sync_mount_unavailable\"") ||
        !guarded_error.str().contains("not currently mounted")) {
        return fail(
            "missing sync mount was not blocked before synchronization"
        );
    }

    const auto dry_root = temporary.path() / "dry";
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("file", "Documents/file.txt", 4)};
    dry_graph.contents["file"] = "data";
    FakeItemStore dry_items;
    FakeMetrics dry_metrics;
    const auto dry_config = config_for(dry_root, true);
    if (onedrive::sync::SyncEngine{
            dry_config, dry_graph, dry_items, dry_metrics, &default_console
        }
                .synchronize() != exit_code::success ||
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
    if (onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
                .synchronize() != exit_code::success ||
        graph.download_count != 1 || items.upsert_count != 1 ||
        items.apply_count != 1 || !metrics.last_success ||
        !progress_output.str().contains("DL: 0/1 files, 50% (2 B/4 B)") ||
        !progress_output.str().contains("Done: 1/1 files, 100% (4 B/4 B)") ||
        !progress_error.str().empty()) {
        return fail("successful download did not commit synchronization state");
    }
    std::ifstream input{root / "Documents/file.txt", std::ios::binary};
    std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}
    };
    if (contents != "data" || items.applied_delta.upserts.size() != 2 ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace ||
        items.applied_delta.upserts[1].local_size != 4 ||
        items.applied_delta.upserts[1].local_modified_ticks == 0) {
        return fail("downloaded file or local snapshot was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_invalid_delta_cursor_restarts_full_query() {
    const onedrive::cli::Console default_console;
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
    items.saved_delta_link = "https://graph.example.test/delta?token=expired";
    FakeMetrics metrics;

    if (onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{
                items.saved_delta_link,
                std::nullopt,
            } ||
        items.apply_count != 1 ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace ||
        items.applied_delta.upserts.size() != 1 || !metrics.last_success) {
        return fail(
            "invalid delta cursor did not restart a replacing full query"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::base_cases
