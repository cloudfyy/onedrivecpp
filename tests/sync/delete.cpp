#include "support.hpp"

namespace {

using namespace onedrive::test::sync;

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
        "deleted", tracked_item(unchanged_root, "deleted", "deleted.txt")
    );
    FakeMetrics unchanged_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(unchanged_root, false),
            unchanged_graph,
            unchanged_items,
            unchanged_metrics
        }
                .synchronize() != 0 ||
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
        "deleted", tracked_item(dry_root, "deleted", "deleted.txt")
    );
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true), dry_graph, dry_items, dry_metrics
        }
                .synchronize() != 0 ||
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
        "gone", tracked_item(refreshed_root, "gone", "gone.txt")
    );
    FakeMetrics refreshed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(refreshed_root, false),
            refreshed_graph,
            refreshed_items,
            refreshed_metrics
        }
                .synchronize() != 0 ||
        std::filesystem::exists(refreshed_root / "gone.txt") ||
        refreshed_items.applied_delta.removals !=
            std::vector<std::string>{"gone"} ||
        refreshed_items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace) {
        return fail("full remote refresh did not reconcile a disappeared item");
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
            modified_config, modified_graph, modified_items, modified_metrics
        }
                .synchronize() != 2 ||
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
            retry_metrics
        }
                .synchronize() != 0 ||
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
            config_for(tree_root, false), tree_graph, tree_items, tree_metrics
        }
                .synchronize() != 0 ||
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
        "folder", tracked_item(nonempty_root, "folder", "Folder", true)
    );
    FakeGraphClient nonempty_graph;
    nonempty_graph.changes = {deleted_item("folder")};
    FakeMetrics nonempty_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nonempty_root, false),
            nonempty_graph,
            nonempty_items,
            nonempty_metrics
        }
                .synchronize() != 2 ||
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
            symlink_metrics
        }
                .synchronize() != 2 ||
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
        }
                .synchronize() != 0 ||
        missing_items.applied_delta.removals !=
            std::vector<std::string>{"missing"}) {
        return fail(
            "already absent remote deletion did not clear its snapshot"
        );
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
        }
                .synchronize() != 0 ||
        untracked_items.applied_delta.removals !=
            std::vector<std::string>{"untracked"}) {
        return fail("untracked remote deletion did not clear stale state");
    }
    return EXIT_SUCCESS;
}

int test_local_deletions() {
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "deletions";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "retained.txt"};
        output << "retained";
    }
    {
        std::ofstream output{root / "removed.txt"};
        output << "removed";
    }
    std::filesystem::create_directories(root / "Removed");
    {
        std::ofstream output{root / "Removed" / "child.txt"};
        output << "child";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "removed-file", tracked_item(root, "removed-file", "removed.txt")
    );
    items.items.emplace(
        "removed-directory",
        tracked_item(root, "removed-directory", "Removed", true)
    );
    items.items.emplace(
        "removed-child",
        tracked_item(root, "removed-child", "Removed/child.txt")
    );
    items.items.emplace(
        "retained", tracked_item(root, "retained", "retained.txt")
    );
    std::filesystem::remove(root / "removed.txt");
    std::filesystem::remove_all(root / "Removed");
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(
        onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize()
    );
    if (graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"removed-directory", "etag"},
                {"removed-file", "etag"},
            } ||
        items.find("me", "removed-directory") ||
        items.find("me", "removed-child") || items.find("me", "removed-file") ||
        !items.find("me", "retained") || !items.pending_deletes_by_id.empty() ||
        !metrics.last_success) {
        return fail("local deletions were not propagated parent-first");
    }

    const auto dry_root = temporary.path() / "dry-deletion";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "removed.txt"};
        output << "removed";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace(
        "dry-removed", tracked_item(dry_root, "dry-removed", "removed.txt")
    );
    std::filesystem::remove(dry_root / "removed.txt");
    FakeGraphClient dry_graph;
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        dry_config, dry_graph, dry_items, dry_metrics
    }
                          .synchronize());
    if (!dry_graph.deleted_items.empty() ||
        !dry_items.pending_deletes_by_id.empty() ||
        !dry_items.find("me", "dry-removed")) {
        return fail("deletion dry run changed remote or local state");
    }

    const auto add_missing_tree = [](FakeItemStore& store,
                                     const std::filesystem::path& tree_root) {
        std::filesystem::create_directories(tree_root / "Missing");
        {
            std::ofstream output{tree_root / "Missing" / "a.txt"};
            output << "aaaa";
        }
        {
            std::ofstream output{tree_root / "Missing" / "b.txt"};
            output << "bbbb";
        }
        store.saved_delta_link = "saved";
        store.items.emplace(
            "guard-directory",
            tracked_item(tree_root, "guard-directory", "Missing", true)
        );
        store.items.emplace(
            "guard-child-a",
            tracked_item(tree_root, "guard-child-a", "Missing/a.txt")
        );
        store.items.emplace(
            "guard-child-b",
            tracked_item(tree_root, "guard-child-b", "Missing/b.txt")
        );
        std::filesystem::remove_all(tree_root / "Missing");
    };

    const auto guarded_root = temporary.path() / "guarded-deletion";
    std::filesystem::create_directories(guarded_root);
    FakeItemStore guarded_items;
    add_missing_tree(guarded_items, guarded_root);
    FakeGraphClient guarded_graph;
    FakeMetrics guarded_metrics;
    auto guarded_config = config_for(guarded_root, false);
    guarded_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    guarded_config.maximum_remote_deletions = 2;
    std::ostringstream guarded_output;
    std::ostringstream guarded_error;
    const onedrive::cli::Console guarded_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
            .quiet = false,
        },
        guarded_output,
        guarded_error
    };
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                              .synchronize());
        return fail("large remote deletion was not blocked");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("large-delete safeguard")) {
            return fail("large remote deletion reported the wrong failure");
        }
    }
    if (!guarded_graph.deleted_items.empty() ||
        !guarded_items.pending_deletes_by_id.empty() ||
        guarded_items.items.size() != 3 || guarded_metrics.last_success ||
        !guarded_output.str().contains("\"delete_operations\":\"1\"") ||
        !guarded_output.str().contains("\"affected_items\":\"3\"") ||
        !guarded_error.str().contains("\"event\":\"large_delete_blocked\"")) {
        return fail(
            "large-delete guard did not preserve state and report its plan"
        );
    }

    FakeItemStore guarded_dry_items;
    add_missing_tree(guarded_dry_items, guarded_root);
    FakeGraphClient guarded_dry_graph;
    FakeMetrics guarded_dry_metrics;
    auto guarded_dry_config = config_for(guarded_root, true);
    guarded_dry_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    guarded_dry_config.maximum_remote_deletions = 2;
    std::ostringstream guarded_dry_output;
    std::ostringstream guarded_dry_error;
    const onedrive::cli::Console guarded_dry_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
            .quiet = false,
        },
        guarded_dry_output,
        guarded_dry_error
    };
    if (onedrive::sync::SyncEngine{
            guarded_dry_config,
            guarded_dry_graph,
            guarded_dry_items,
            guarded_dry_metrics,
            &guarded_dry_console
        }
                .synchronize() != 0 ||
        !guarded_dry_graph.deleted_items.empty() ||
        guarded_dry_items.items.size() != 3 ||
        !guarded_dry_output.str().contains(
            "\"event\":\"large_delete_detected\""
        ) ||
        !guarded_dry_output.str().contains(
            "\"large_delete_blocked\":\"true\""
        )) {
        return fail(
            "large-delete dry run did not report without changing state"
        );
    }

    guarded_config.force_large_delete = true;
    guarded_output.str({});
    guarded_error.str({});
    if (onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                .synchronize() != 0 ||
        guarded_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"guard-directory", "etag"},
            } ||
        !guarded_items.items.empty() || !guarded_metrics.last_success ||
        !guarded_output.str().contains("\"event\":\"large_delete_forced\"")) {
        return fail("explicit large-delete override did not execute the plan");
    }

    const auto pending_guard_root =
        temporary.path() / "pending-guarded-deletion";
    std::filesystem::create_directories(pending_guard_root);
    FakeItemStore pending_guard_items;
    add_missing_tree(pending_guard_items, pending_guard_root);
    pending_guard_items.pending_deletes_by_id.emplace(
        "guard-directory",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "guard-directory",
            .expected_etag = "etag",
            .remote_path = "Missing",
            .local_path = pending_guard_root / "Missing",
            .directory = true,
        }
    );
    FakeGraphClient pending_guard_graph;
    FakeMetrics pending_guard_metrics;
    auto pending_guard_config = config_for(pending_guard_root, false);
    pending_guard_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    pending_guard_config.maximum_remote_deletions = 2;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            pending_guard_config,
            pending_guard_graph,
            pending_guard_items,
            pending_guard_metrics
        }
                              .synchronize());
        return fail("pending deletions bypassed the large-delete guard");
    } catch (const std::runtime_error&) {
    }
    if (!pending_guard_graph.deleted_items.empty() ||
        pending_guard_items.pending_deletes_by_id.size() != 1 ||
        pending_guard_items.items.size() != 3 ||
        pending_guard_metrics.last_success) {
        return fail("blocked pending deletions changed remote or local state");
    }

    const auto selective_root = temporary.path() / "selective-deletion";
    std::filesystem::create_directories(selective_root / "Included");
    std::filesystem::create_directories(selective_root / "Excluded");
    {
        std::ofstream output{selective_root / "Included" / "removed.txt"};
        output << "included";
    }
    {
        std::ofstream output{selective_root / "Excluded" / "retained.txt"};
        output << "excluded";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    selective_items.items.emplace(
        "included-delete",
        tracked_item(selective_root, "included-delete", "Included/removed.txt")
    );
    selective_items.items.emplace(
        "excluded-delete",
        tracked_item(selective_root, "excluded-delete", "Excluded/retained.txt")
    );
    std::filesystem::remove(selective_root / "Included" / "removed.txt");
    std::filesystem::remove(selective_root / "Excluded" / "retained.txt");
    const auto sync_list = temporary.path() / "delete-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeGraphClient selective_graph;
    FakeMetrics selective_metrics;
    auto selective_config = config_for(selective_root, false);
    selective_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    selective_config.sync_list = sync_list;
    selective_items.saved_sync_filter_fingerprint =
        onedrive::sync::detail::SyncList::load(
            sync_list,
            onedrive::sync::detail::root_file_policy(
                selective_config.sync_root_files
            )
        )
            .fingerprint();
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config, selective_graph, selective_items, selective_metrics
    }
                          .synchronize());
    if (selective_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"included-delete", "etag"},
            } ||
        selective_items.find("me", "included-delete") ||
        !selective_items.find("me", "excluded-delete")) {
        return fail("selective sync deletion boundary was not preserved");
    }

    const auto policy_root = temporary.path() / "policy-deletion";
    std::filesystem::create_directories(policy_root / "Ignored");
    {
        std::ofstream marker{policy_root / "Ignored" / ".nosync"};
    }
    {
        std::ofstream output{policy_root / ".hidden"};
        output << "data";
    }
    {
        std::ofstream output{policy_root / "large.bin"};
        output << "large";
    }
    {
        std::ofstream output{policy_root / "Ignored" / "file.txt"};
        output << "data";
    }
    FakeItemStore policy_items;
    policy_items.saved_delta_link = "saved";
    policy_items.items.emplace(
        "hidden-delete",
        tracked_item(policy_root, "hidden-delete", ".hidden")
    );
    auto large_item =
        tracked_item(policy_root, "large-delete", "large.bin");
    large_item.size = 5;
    policy_items.items.emplace("large-delete", std::move(large_item));
    policy_items.items.emplace(
        "nosync-delete",
        tracked_item(policy_root, "nosync-delete", "Ignored/file.txt")
    );
    std::filesystem::remove(policy_root / ".hidden");
    std::filesystem::remove(policy_root / "large.bin");
    std::filesystem::remove(policy_root / "Ignored" / "file.txt");
    FakeGraphClient policy_graph;
    FakeMetrics policy_metrics;
    auto policy_config = config_for(policy_root, false);
    policy_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    policy_config.dotfiles = onedrive::config::DotfilePolicy::exclude;
    policy_config.maximum_file_size_bytes = 4;
    policy_items.saved_sync_filter_fingerprint =
        onedrive::sync::detail::SyncList::configured({
            .sync_root = policy_root,
            .include_root_files = policy_config.sync_root_files,
            .nosync_enabled = policy_config.nosync_enabled,
            .dotfiles = policy_config.dotfiles,
            .maximum_file_size_bytes =
                policy_config.maximum_file_size_bytes,
        })
            .fingerprint();
    static_cast<void>(onedrive::sync::SyncEngine{
        policy_config, policy_graph, policy_items, policy_metrics
    }
                          .synchronize());
    if (!policy_graph.deleted_items.empty() || policy_items.size() != 3) {
        return fail("filtered local absence propagated remote deletions");
    }

    const auto journal_failure_root =
        temporary.path() / "delete-journal-failure";
    std::filesystem::create_directories(journal_failure_root);
    {
        std::ofstream output{journal_failure_root / "removed.txt"};
        output << "removed";
    }
    FakeItemStore journal_failure_items;
    journal_failure_items.saved_delta_link = "saved";
    journal_failure_items.items.emplace(
        "journal-failure-delete",
        tracked_item(
            journal_failure_root, "journal-failure-delete", "removed.txt"
        )
    );
    std::filesystem::remove(journal_failure_root / "removed.txt");
    journal_failure_items.fail_pending_delete_save = true;
    FakeGraphClient journal_failure_graph;
    FakeMetrics journal_failure_metrics;
    auto journal_failure_config = config_for(journal_failure_root, false);
    journal_failure_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            journal_failure_config,
            journal_failure_graph,
            journal_failure_items,
            journal_failure_metrics
        }
                              .synchronize());
        return fail("deletion journal failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!journal_failure_graph.deleted_items.empty() ||
        !journal_failure_items.pending_deletes_by_id.empty() ||
        !journal_failure_items.find("me", "journal-failure-delete")) {
        return fail("unjournaled deletion reached Microsoft Graph");
    }
    journal_failure_items.fail_pending_delete_save = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_failure_config,
        journal_failure_graph,
        journal_failure_items,
        journal_failure_metrics
    }
                          .synchronize());
    if (journal_failure_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"journal-failure-delete", "etag"},
            } ||
        journal_failure_items.find("me", "journal-failure-delete") ||
        !journal_failure_items.pending_deletes_by_id.empty()) {
        return fail("deletion did not recover after journal failure");
    }

    const auto recovery_root = temporary.path() / "delete-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "recover.txt"};
        output << "recover";
    }
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.items.emplace(
        "recover-delete",
        tracked_item(recovery_root, "recover-delete", "recover.txt")
    );
    std::filesystem::remove(recovery_root / "recover.txt");
    recovery_items.fail_commit_delete = true;
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config, recovery_graph, recovery_items, recovery_metrics
        }
                              .synchronize());
        return fail("deletion commit failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_deletes_by_id.size() != 1 ||
        !recovery_items.find("me", "recover-delete")) {
        return fail("deletion commit failure did not retain its journal");
    }
    recovery_items.fail_commit_delete = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config, recovery_graph, recovery_items, recovery_metrics
    }
                          .synchronize());
    if (recovery_graph.deleted_items.size() != 2 ||
        recovery_items.find("me", "recover-delete") ||
        !recovery_items.pending_deletes_by_id.empty()) {
        return fail("pending deletion was not recovered idempotently");
    }

    const auto reappeared_root = temporary.path() / "delete-reappeared";
    std::filesystem::create_directories(reappeared_root);
    {
        std::ofstream output{reappeared_root / "restored.txt"};
        output << "restored";
    }
    FakeItemStore reappeared_items;
    reappeared_items.saved_delta_link = "saved";
    const auto restored =
        tracked_item(reappeared_root, "restored", "restored.txt");
    reappeared_items.items.emplace("restored", restored);
    reappeared_items.pending_deletes_by_id.emplace(
        "restored",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "restored",
            .expected_etag = restored.etag,
            .remote_path = restored.remote_path,
            .local_path = restored.local_path,
        }
    );
    FakeGraphClient reappeared_graph;
    FakeMetrics reappeared_metrics;
    auto reappeared_config = config_for(reappeared_root, false);
    reappeared_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            reappeared_config,
            reappeared_graph,
            reappeared_items,
            reappeared_metrics
        }
                              .synchronize());
        return fail("reappeared local item was deleted remotely");
    } catch (const std::runtime_error&) {
    }
    if (!reappeared_graph.deleted_items.empty() ||
        !reappeared_items.pending_deletes_by_id.empty() ||
        !reappeared_items.find("me", "restored")) {
        return fail("reappeared local item did not cancel deletion recovery");
    }

    const auto conflict_root = temporary.path() / "delete-conflict";
    std::filesystem::create_directories(conflict_root);
    {
        std::ofstream output{conflict_root / "conflict.txt"};
        output << "conflict";
    }
    FakeItemStore conflict_items;
    conflict_items.saved_delta_link = "saved";
    conflict_items.items.emplace(
        "conflict-delete",
        tracked_item(conflict_root, "conflict-delete", "conflict.txt")
    );
    std::filesystem::remove(conflict_root / "conflict.txt");
    FakeGraphClient conflict_graph;
    conflict_graph.delete_conflict = true;
    FakeMetrics conflict_metrics;
    auto conflict_config = config_for(conflict_root, false);
    conflict_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            conflict_config, conflict_graph, conflict_items, conflict_metrics
        }
                              .synchronize());
        return fail("remote deletion conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!conflict_items.pending_deletes_by_id.empty() ||
        !conflict_items.find("me", "conflict-delete") ||
        conflict_metrics.last_success) {
        return fail("definite deletion conflict retained recovery state");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_remote_deletions(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_deletions(); result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
