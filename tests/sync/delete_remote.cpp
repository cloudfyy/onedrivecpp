#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace onedrive::test::sync::delete_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_delete_unchanged() {
    const onedrive::cli::Console default_console;
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
        "deleted", tracked_item(unchanged_root, "deleted", "deleted.txt")
    );
    FakeMetrics unchanged_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(unchanged_root, false),
            unchanged_graph,
            unchanged_items,
            unchanged_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(unchanged_root / "deleted.txt") ||
        unchanged_items.applied_delta.removals !=
            std::vector<std::string>{"deleted"} ||
        unchanged_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"deleted"} ||
        !unchanged_items.applied_delta.blocked_upserts.empty() ||
        !unchanged_metrics.last_success) {
        return fail("unchanged remote deletion was not executed safely");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_preview() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
        "deleted", tracked_item(dry_root, "deleted", "deleted.txt")
    );
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true),
            dry_graph,
            dry_items,
            dry_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(dry_root / "deleted.txt") ||
        dry_items.apply_count != 0) {
        return fail("remote deletion dry run changed local state");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_full_refresh() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto refreshed_root = temporary.path() / "full-refresh";
    std::filesystem::create_directories(refreshed_root);
    {
        std::ofstream output{refreshed_root / "gone.txt"};
        output << "data";
    }
    FakeGraphClient refreshed_graph;
    FakeItemStore refreshed_items;
    refreshed_items.items.emplace(
        "gone", tracked_item(refreshed_root, "gone", "gone.txt")
    );
    FakeMetrics refreshed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(refreshed_root, false),
            refreshed_graph,
            refreshed_items,
            refreshed_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(refreshed_root / "gone.txt") ||
        refreshed_items.applied_delta.removals !=
            std::vector<std::string>{"gone"} ||
        refreshed_items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace) {
        return fail("full remote refresh did not reconcile a disappeared item");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_modified_retry() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto modified_root = temporary.path() / "modified";
    std::filesystem::create_directories(modified_root);
    {
        std::ofstream output{modified_root / "modified.txt"};
        output << "data";
    }
    FakeItemStore modified_items;
    modified_items.saved_delta_link = "saved";
    modified_items.items.emplace(
        "modified", tracked_item(modified_root, "modified", "modified.txt")
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
            modified_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
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
    retry_items.blocked = modified_items.applied_delta.blocked_upserts;
    FakeMetrics retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(modified_root, false),
            retry_graph,
            retry_items,
            retry_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        retry_items.applied_delta.removals !=
            std::vector<std::string>{"modified"} ||
        retry_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"modified"} ||
        !retry_items.applied_delta.blocked_upserts.empty()) {
        return fail("blocked remote deletion was not retried successfully");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_tree() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto tree_root = temporary.path() / "tree";
    std::filesystem::create_directories(tree_root / "Folder");
    {
        std::ofstream output{tree_root / "Folder" / "child.txt"};
        output << "data";
    }
    FakeItemStore tree_items;
    tree_items.saved_delta_link = "saved";
    tree_items.items.emplace(
        "folder", tracked_item(tree_root, "folder", "Folder", true)
    );
    tree_items.items.emplace(
        "child", tracked_item(tree_root, "child", "Folder/child.txt")
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
            tree_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(tree_root / "Folder") ||
        tree_items.applied_delta.removals.size() != 2) {
        return fail("remote deletion did not remove children before parents");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_nonempty_directory() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto nonempty_root = temporary.path() / "nonempty";
    std::filesystem::create_directories(nonempty_root / "Folder");
    {
        std::ofstream output{nonempty_root / "Folder" / "local.txt"};
        output << "local";
    }
    FakeItemStore nonempty_items;
    nonempty_items.saved_delta_link = "saved";
    nonempty_items.items.emplace(
        "folder", tracked_item(nonempty_root, "folder", "Folder", true)
    );
    FakeGraphClient nonempty_graph;
    nonempty_graph.changes = {deleted_item("folder")};
    FakeMetrics nonempty_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nonempty_root, false),
            nonempty_graph,
            nonempty_items,
            nonempty_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::exists(nonempty_root / "Folder" / "local.txt") ||
        nonempty_items.applied_delta.blocked_upserts.size() != 1 ||
        nonempty_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("nonempty remotely deleted directory was not blocked");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_symlink() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto symlink_root = temporary.path() / "delete-symlink";
    const auto outside = temporary.path() / "delete-outside.txt";
    std::filesystem::create_directories(symlink_root);
    {
        std::ofstream output{outside};
        output << "data";
    }
    std::filesystem::create_symlink(outside, symlink_root / "linked.txt");
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
            symlink_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::is_symlink(symlink_root / "linked.txt") ||
        !std::filesystem::exists(outside) ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote deletion followed or removed a symbolic link");
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_already_absent() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
            missing_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        missing_items.applied_delta.removals !=
            std::vector<std::string>{"missing"}) {
        return fail(
            "already absent remote deletion did not clear its snapshot"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_delete_untracked() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
            untracked_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        untracked_items.applied_delta.removals !=
            std::vector<std::string>{"untracked"}) {
        return fail("untracked remote deletion did not clear stale state");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::delete_cases
