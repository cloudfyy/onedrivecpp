#include "upload_support.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_upload_progress_pipeline() {
    for (const bool blocked : {false, true}) {
        onedrive::test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "uploads";
        std::filesystem::create_directory(root);
        onedrive::test::write_file(root / "file.txt", "payload");
        if (!blocked) {
            onedrive::test::write_file(root / "empty.txt", "");
        }
        FakeGraphClient graph;
        if (blocked) {
            graph.upload_resource_error_path = "file.txt";
        }
        FakeItemStore items;
        items.saved_delta_link = "saved";
        FakeMetrics metrics;
        auto config = config_for(root, false);
        config.sync_mode = onedrive::sync::SyncMode::upload_only;
        TransferObserver observer;
        const auto result =
            onedrive::sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize();
        if (observer.progress.empty() || !observer.summary ||
            result != (blocked ? exit_code::blocked : exit_code::success) ||
            observer.summary->uploaded != (blocked ? 0U : 2U) ||
            observer.summary->blocked != (blocked ? 1U : 0U)) {
            return fail(
                "upload pipeline did not emit typed progress and round counts"
            );
        }
        const auto& final = observer.progress.back();
        if (blocked) {
            if (std::ranges::any_of(observer.progress, [](const auto& event) {
                    return event.state ==
                           onedrive::util::ProgressState::completed;
                })) {
                return fail(
                    "resource-blocked upload emitted successful batch "
                    "completion"
                );
            }
        } else if (final.file_count != 2 || final.completed_files != 2 ||
                   final.uploaded != 7 || final.total != 7 ||
                   final.state != onedrive::util::ProgressState::completed) {
            return fail(
                "upload progress lost concurrency or zero-byte file completion"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_upload_preview() {
    const onedrive::cli::Console default_console;
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
    dry_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            dry_config, dry_graph, dry_items, dry_metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        dry_graph.upload_count != 0 || dry_items.apply_count != 0) {
        return fail("upload dry run changed remote or local state");
    }
    return EXIT_SUCCESS;
}

int test_new_and_modified_uploads() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
        "modified", tracked_item(root, "modified", "modified.txt")
    );
    items.items.emplace(
        "unchanged", tracked_item(root, "unchanged", "unchanged.txt")
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
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const auto result =
        onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
            .synchronize();
    std::ranges::sort(graph.uploaded_paths);
    bool upload_snapshot_found = false;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().contains(".onedrive-upload-")) {
            upload_snapshot_found = true;
        }
    }
    const auto modified = items.find("me", "modified");
    const auto unchanged = items.find("me", "unchanged");
    if (result != exit_code::blocked || graph.upload_count != 2 ||
        items.blocked.size() != 2 ||
        std::ranges::any_of(
            items.blocked,
            [](const auto& item) {
                return onedrive::sync::blocked_item_strategy(item.reason_code)
                           .traits()
                           .origin != onedrive::sync::BlockedOrigin::local;
            }
        ) ||
        graph.uploaded_paths !=
            std::vector<std::string>{"modified.txt", "new.txt"} ||
        !modified ||
        (modified->etag != "uploaded-etag-1" &&
         modified->etag != "uploaded-etag-2") ||
        modified->local_size != 7 || modified->local_device == 0 ||
        modified->local_inode == 0 || !unchanged ||
        unchanged->local_device == 0 || unchanged->local_inode == 0 ||
        (!items.find("me", "uploaded-1") && !items.find("me", "uploaded-2")) ||
        upload_snapshot_found || !metrics.last_success) {
        return fail("new and modified local files were not uploaded safely");
    }
    return EXIT_SUCCESS;
}

int test_upload_concurrency() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto concurrent_root = temporary.path() / "concurrent-uploads";
    std::filesystem::create_directories(concurrent_root);
    for (int index = 0; index < 4; ++index) {
        std::ofstream{
            concurrent_root / ("file-" + std::to_string(index) + ".txt")
        } << "parallel";
    }
    FakeItemStore concurrent_items;
    concurrent_items.saved_delta_link = "saved";
    FakeGraphClient concurrent_graph;
    concurrent_graph.upload_delay = std::chrono::milliseconds{50};
    FakeMetrics concurrent_metrics;
    auto concurrent_config = config_for(concurrent_root, false);
    concurrent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    concurrent_config.upload_concurrency = 2;
    const auto concurrent_result =
        onedrive::sync::SyncEngine{
            concurrent_config,
            concurrent_graph,
            concurrent_items,
            concurrent_metrics,
            &default_console
        }
            .synchronize();
    if (concurrent_result != exit_code::success ||
        concurrent_graph.upload_count != 4 ||
        concurrent_graph.maximum_concurrent_uploads != 2 ||
        concurrent_items.size() != 4 || !concurrent_metrics.last_success) {
        return fail(
            std::format(
                "uploads did not respect the configured concurrency: "
                "result={}, count={}, maximum={}, items={}, success={}",
                concurrent_result,
                concurrent_graph.upload_count,
                concurrent_graph.maximum_concurrent_uploads.load(),
                concurrent_items.size(),
                concurrent_metrics.last_success
            )
        );
    }
    return EXIT_SUCCESS;
}

int test_fatal_upload_stops_workers() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto cancelled_root =
        temporary.path() / "cancelled-concurrent-uploads";
    std::filesystem::create_directories(cancelled_root);
    for (int index = 0; index < 6; ++index) {
        std::ofstream{
            cancelled_root / ("file-" + std::to_string(index) + ".txt")
        } << "cancel";
    }
    FakeItemStore cancelled_items;
    cancelled_items.saved_delta_link = "saved";
    FakeGraphClient cancelled_graph;
    cancelled_graph.upload_delay = std::chrono::milliseconds{30};
    cancelled_graph.fatal_upload_error = true;
    FakeMetrics cancelled_metrics;
    auto cancelled_config = config_for(cancelled_root, false);
    cancelled_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    cancelled_config.upload_concurrency = 2;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            cancelled_config,
            cancelled_graph,
            cancelled_items,
            cancelled_metrics,
            &default_console
        }
                              .synchronize());
        return fail("fatal parallel upload failure was ignored");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "simulated fatal upload failure"
            )) {
            return fail("parallel upload replaced the fatal error");
        }
    }
    if (cancelled_graph.upload_count > 2 || cancelled_metrics.last_success) {
        return fail(
            "parallel uploads continued taking work after a fatal failure"
        );
    }
    return EXIT_SUCCESS;
}

int test_local_change_during_upload() {
    const onedrive::cli::Console default_console;
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
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.upload_count != 1) {
        return fail("initial changing local file upload failed");
    }
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.upload_count != 2) {
        return fail("local change during upload was marked as synchronized");
    }
    const auto uploaded = items.find("me", "uploaded-1");
    if (!uploaded || uploaded->local_size != 12 ||
        !items.pending_uploads_by_path.empty() || !metrics.last_success) {
        return fail("follow-up upload did not commit the changed local file");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::upload_cases
