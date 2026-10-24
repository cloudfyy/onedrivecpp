#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace onedrive::test::sync::delete_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_delete_partial_cleanup() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto partial_root = temporary.path() / "partial-cleanup";
    std::filesystem::create_directories(partial_root);
    const auto removed_partial = partial_root / ".removed.partial";
    const auto child_partial = partial_root / ".child.partial";
    const auto retained_partial = partial_root / ".retained.partial";
    const auto outside_partial_target =
        temporary.path() / "partial-symlink-target";
    {
        std::ofstream{removed_partial} << "partial";
        std::ofstream{child_partial} << "partial";
        std::ofstream{retained_partial} << "partial";
        std::ofstream{outside_partial_target} << "outside";
    }
    const auto symlink_partial = partial_root / ".symlink.partial";
    std::filesystem::create_symlink(outside_partial_target, symlink_partial);
    FakeGraphClient partial_graph;
    partial_graph.changes = {
        deleted_item("removed-partial"),
        deleted_item("missing-partial"),
        deleted_item("removed-folder"),
        deleted_item("symlink-partial"),
    };
    FakeItemStore partial_items;
    partial_items.saved_delta_link = "saved";
    const auto add_partial = [&partial_items, &partial_root](
                                 std::string remote_id,
                                 std::string parent_id,
                                 const std::filesystem::path& temporary_path
                             ) {
        partial_items.partials.emplace(
            remote_id,
            onedrive::storage::PartialDownload{
                .item =
                    {
                        .drive_id = "me",
                        .remote_id = remote_id,
                        .parent_id = std::move(parent_id),
                        .name = remote_id + ".txt",
                        .etag = "etag",
                        .remote_path = remote_id + ".txt",
                        .local_path = partial_root / (remote_id + ".txt"),
                        .size = 7,
                    },
                .temporary_path = temporary_path,
                .completed_bytes = 7,
            }
        );
    };
    add_partial("removed-partial", "root", removed_partial);
    add_partial("missing-partial", "root", partial_root / ".missing.partial");
    add_partial("child-partial", "removed-folder", child_partial);
    add_partial("retained-partial", "root", retained_partial);
    add_partial("symlink-partial", "root", symlink_partial);
    FakeMetrics partial_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(partial_root, false),
            partial_graph,
            partial_items,
            partial_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(removed_partial) ||
        std::filesystem::exists(child_partial) ||
        std::filesystem::exists(symlink_partial) ||
        !std::filesystem::exists(outside_partial_target) ||
        !std::filesystem::exists(retained_partial) ||
        partial_items.partials.size() != 1 ||
        !partial_items.partials.contains("retained-partial") ||
        partial_items.applied_delta.partial_download_removals.size() != 4) {
        return fail(
            "remote deletions did not clean orphaned partial downloads"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_orphan_partial() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto refresh_partial_root =
        temporary.path() / "refresh-partial-cleanup";
    std::filesystem::create_directories(refresh_partial_root);
    const auto refresh_partial = refresh_partial_root / ".orphan.partial";
    std::ofstream{refresh_partial} << "partial";
    FakeGraphClient refresh_partial_graph;
    FakeItemStore refresh_partial_items;
    refresh_partial_items.partials.emplace(
        "orphan",
        onedrive::storage::PartialDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "orphan",
                    .parent_id = "root",
                    .name = "orphan.txt",
                    .etag = "etag",
                    .remote_path = "orphan.txt",
                    .local_path = refresh_partial_root / "orphan.txt",
                    .size = 7,
                },
            .temporary_path = refresh_partial,
            .completed_bytes = 7,
        }
    );
    FakeMetrics refresh_partial_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(refresh_partial_root, false),
            refresh_partial_graph,
            refresh_partial_items,
            refresh_partial_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(refresh_partial) ||
        !refresh_partial_items.partials.empty() ||
        refresh_partial_items.applied_delta.partial_download_removals !=
            std::vector<std::string>{"orphan"}) {
        return fail("full refresh did not clean an orphaned partial download");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_live_partial() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto retained_refresh_root =
        temporary.path() / "retained-refresh-partial";
    std::filesystem::create_directories(retained_refresh_root);
    const auto retained_refresh_partial =
        retained_refresh_root / ".retained.partial";
    std::ofstream{retained_refresh_partial} << "part";
    FakeGraphClient retained_refresh_graph;
    retained_refresh_graph.changes = {
        file("retained-refresh", "retained.txt", 7),
    };
    FakeItemStore retained_refresh_items;
    retained_refresh_items.partials.emplace(
        "retained-refresh",
        onedrive::storage::PartialDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "retained-refresh",
                    .parent_id = "root",
                    .name = "retained.txt",
                    .etag = "etag",
                    .remote_path = "retained.txt",
                    .local_path = retained_refresh_root / "retained.txt",
                    .size = 7,
                },
            .temporary_path = retained_refresh_partial,
            .completed_bytes = 4,
        }
    );
    FakeMetrics retained_refresh_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(retained_refresh_root, true),
            retained_refresh_graph,
            retained_refresh_items,
            retained_refresh_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(retained_refresh_partial) ||
        !retained_refresh_items.partials.contains("retained-refresh") ||
        retained_refresh_items.apply_count != 0) {
        return fail("full refresh discarded a still-live partial download");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_unsafe_partial() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto unsafe_partial_root =
        temporary.path() / "unsafe-partial-cleanup";
    const auto unsafe_partial_outside =
        temporary.path() / "unsafe-partial-outside";
    std::filesystem::create_directories(unsafe_partial_root);
    std::filesystem::create_directories(unsafe_partial_outside);
    const auto unsafe_partial_file = unsafe_partial_outside / ".unsafe.partial";
    std::ofstream{unsafe_partial_file} << "partial";
    std::filesystem::create_directory_symlink(
        unsafe_partial_outside, unsafe_partial_root / "linked"
    );
    FakeGraphClient unsafe_partial_graph;
    unsafe_partial_graph.changes = {deleted_item("unsafe-partial")};
    FakeItemStore unsafe_partial_items;
    unsafe_partial_items.saved_delta_link = "saved";
    unsafe_partial_items.partials.emplace(
        "unsafe-partial",
        onedrive::storage::PartialDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "unsafe-partial",
                    .parent_id = "root",
                    .name = "unsafe.txt",
                    .etag = "etag",
                    .remote_path = "unsafe.txt",
                    .local_path = unsafe_partial_root / "unsafe.txt",
                    .size = 7,
                },
            .temporary_path =
                unsafe_partial_root / "linked" / ".unsafe.partial",
            .completed_bytes = 7,
        }
    );
    FakeMetrics unsafe_partial_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(unsafe_partial_root, false),
            unsafe_partial_graph,
            unsafe_partial_items,
            unsafe_partial_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(unsafe_partial_file) ||
        !unsafe_partial_items.partials.empty()) {
        return fail("partial cleanup followed an unsafe parent symlink");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_partial_commit_failure() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto failed_partial_root =
        temporary.path() / "failed-partial-cleanup";
    std::filesystem::create_directories(failed_partial_root);
    const auto failed_partial = failed_partial_root / ".failed.partial";
    std::ofstream{failed_partial} << "partial";
    FakeGraphClient failed_partial_graph;
    failed_partial_graph.changes = {deleted_item("failed-partial")};
    FakeItemStore failed_partial_items;
    failed_partial_items.saved_delta_link = "saved";
    failed_partial_items.fail_apply_delta = true;
    failed_partial_items.partials.emplace(
        "failed-partial",
        onedrive::storage::PartialDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "failed-partial",
                    .parent_id = "root",
                    .name = "failed.txt",
                    .etag = "etag",
                    .remote_path = "failed.txt",
                    .local_path = failed_partial_root / "failed.txt",
                    .size = 7,
                },
            .temporary_path = failed_partial,
            .completed_bytes = 7,
        }
    );
    FakeMetrics failed_partial_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(failed_partial_root, false),
            failed_partial_graph,
            failed_partial_items,
            failed_partial_metrics,
            &default_console
        }
                              .synchronize());
        return fail("delta commit failure did not interrupt partial cleanup");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(failed_partial) ||
        !failed_partial_items.partials.contains("failed-partial")) {
        return fail("failed delta commit removed resumable partial data");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::delete_cases
