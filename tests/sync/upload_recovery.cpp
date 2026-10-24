#include "upload_support.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_pending_upload_recovery() {
    const onedrive::cli::Console default_console;
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
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
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
    TransferObserver observer;
    if (onedrive::sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize() != exit_code::success ||
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
        return fail("completed pending upload was not verified and recovered");
    }
    if (!observer.summary || observer.summary->uploaded != 1 ||
        observer.progress.empty() ||
        observer.progress.back().completed_files != 1 ||
        observer.progress.back().state !=
            onedrive::util::ProgressState::completed) {
        return fail("round upload totals omitted a recovered upload");
    }
    return EXIT_SUCCESS;
}

int test_upload_checkpoint_recovery() {
    const onedrive::cli::Console default_console;
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
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
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
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.upload_count != 2 || graph.upload_sessions.size() != 2 ||
        !graph.upload_sessions[1] ||
        graph.upload_sessions[1]->completed_bytes != 4 ||
        graph.upload_sessions[1]->upload_url !=
            "https://upload.example.test/session?secret=1" ||
        !items.pending_uploads_by_path.empty() || !metrics.last_success) {
        return fail("persisted upload checkpoint was not resumed");
    }

    const auto failing_root = temporary.path() / "upload-checkpoint-failure";
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
    failing_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            failing_config,
            failing_graph,
            failing_items,
            failing_metrics,
            &default_console
        }
                              .synchronize());
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
        !failing_items.pending_uploads_by_path.begin()
             ->second.upload_url.empty()) {
        return fail("failed upload checkpoint was treated as committed");
    }
    return EXIT_SUCCESS;
}

int test_external_cancellation_preserves_upload_journal() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "cancelled-upload";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "new.txt"};
        output << "new upload";
    }
    FakeGraphClient graph;
    graph.upload_checkpoint = onedrive::graph::UploadSession{
        .upload_url = "https://upload.example.test/session",
        .expiration = "2099-10-09T11:00:00Z",
        .completed_bytes = 0,
    };
    std::stop_source stop;
    graph.during_upload = [&] { stop.request_stop(); };
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;

    const int result =
        onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
            .synchronize(stop.get_token());
    const auto pending = items.pending_uploads("me");
    if (result != exit_code::cancelled ||
        metrics.last_outcome != onedrive::metrics::SyncRunOutcome::cancelled ||
        pending.size() != 1 || pending.front().upload_url.empty() ||
        !std::filesystem::exists(pending.front().snapshot_path)) {
        return fail(
            "cancelled upload did not retain its checkpoint journal and "
            "snapshot"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::upload_cases
