#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace onedrive::test::sync::delete_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_recursive_deletion_protection() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    FakeItemStore items;
    items.items.emplace("dir", tracked_item(root, "dir", "Missing", true));
    items.items.emplace("other", tracked_item(root, "other", "Other", true));
    for (const auto* id : {"a", "b"}) {
        items.items.emplace(
            id,
            onedrive::storage::ItemState{
                .drive_id = "me",
                .remote_id = id,
                .remote_path = std::string{"Missing/"} + id,
                .local_path = root / "Missing" / id,
                .size = 4,
            }
        );
    }
    onedrive::storage::ItemStore store{onedrive::util::borrowed_proxy, items};
    auto plan =
        onedrive::sync::detail::discover_deletions("me", store, nullptr);
    if (plan.operations.size() != 2 || plan.affected_items != 4) {
        return fail("recursive deletion did not count the complete subtree");
    }
    items.blocked.push_back({
        .drive_id = "me",
        .remote_id = "a",
        .remote_path = "Missing/a",
        .reason_code = "malware_detected",
    });
    plan = onedrive::sync::detail::discover_deletions("me", store, nullptr);
    if (plan.affected_items != 2 ||
        std::ranges::any_of(plan.operations, [](const auto& deletion) {
            return deletion.remote_id == "dir" || deletion.remote_id == "a";
        })) {
        return fail("parent deletion covered a blocked descendant");
    }
    items.blocked.clear();
    plan =
        onedrive::sync::detail::discover_deletions("me", store, nullptr, {"a"});
    if (std::ranges::any_of(plan.operations, [](const auto& deletion) {
            return deletion.remote_id == "dir" || deletion.remote_id == "a";
        })) {
        return fail("parent deletion covered a moved descendant");
    }
    const auto rules = root / "rules";
    std::ofstream{rules} << "/Missing/\n/Other/\n!/Missing/a\n";
    const auto filter = onedrive::sync::detail::SyncList::load(rules);
    plan = onedrive::sync::detail::discover_deletions("me", store, &filter);
    if (std::ranges::any_of(plan.operations, [](const auto& deletion) {
            return deletion.remote_id == "dir" || deletion.remote_id == "a";
        })) {
        return fail("parent deletion covered an excluded descendant");
    }
    items.blocked.push_back({
        .drive_id = "me",
        .remote_id = "untracked",
        .remote_path = "Missing/untracked",
        .reason_code = "malware_detected",
    });
    items.save_pending_delete({
        .drive_id = "me",
        .remote_id = "dir",
        .remote_path = "Missing",
        .local_path = root / "Missing",
        .directory = true,
    });
    items.save_pending_delete({
        .drive_id = "me",
        .remote_id = "other",
        .remote_path = "Other",
        .local_path = root / "Other",
        .directory = true,
    });
    FakeGraphClient graph;
    onedrive::graph::GraphClient graph_port{
        onedrive::util::borrowed_proxy, graph
    };
    const onedrive::cli::Console console;
    onedrive::sync::detail::recover_pending_deletes(
        "me", graph_port, store, console, {}
    );
    if (graph.deleted_items.size() != 1 ||
        graph.deleted_items.front().first != "other" ||
        !items.pending_deletes_by_id.contains("dir") ||
        !items.find("me", "a")) {
        return fail("pending deletion bypassed a blocked descendant");
    }
    plan = onedrive::sync::detail::discover_deletions("me", store, nullptr);
    if (std::ranges::any_of(plan.operations, [](const auto& deletion) {
            return deletion.remote_id == "dir";
        })) {
        return fail("parent deletion covered an untracked blocked descendant");
    }
    return EXIT_SUCCESS;
}

int test_full_refresh_retains_blocked_deletion_snapshot() {
    const onedrive::cli::Console console;
    TemporaryDirectory temporary;
    for (const bool expired_cursor : {false, true}) {
        const auto root =
            temporary.path() / (expired_cursor ? "expired" : "reset");
        std::filesystem::create_directories(root);
        std::ofstream{root / "file.txt"} << "data";
        FakeItemStore items;
        items.items.emplace("file", tracked_item(root, "file", "file.txt"));
        if (expired_cursor) {
            items.saved_delta_link = "expired";
        }
        std::ofstream{root / "file.txt"} << "modified-local";
        FakeGraphClient graph;
        graph.reject_saved_cursor = expired_cursor;
        FakeMetrics metrics;
        auto config = config_for(root, false);
        config.sync_mode = onedrive::sync::SyncMode::bidirectional;
        if (expired_cursor) {
            items.saved_sync_filter_fingerprint =
                onedrive::sync::engine_detail::sync_scope_fingerprint(
                    onedrive::sync::detail::SyncList::configured(
                        {
                            .rules_path = config.sync_list,
                            .sync_root = root,
                            .include_root_files = config.sync_root_files,
                            .nosync_enabled = config.nosync_enabled,
                            .dotfiles = config.dotfiles,
                            .maximum_file_size_bytes =
                                config.maximum_file_size_bytes,
                        }
                    )
                        .fingerprint()
                );
        }
        const auto synchronize = [&] {
            return onedrive::sync::SyncEngine{
                config, graph, items, metrics, &console
            }
                .synchronize();
        };
        if (synchronize() != exit_code::blocked || !items.find("me", "file") ||
            items.find("me", "file")->local_size != 4 ||
            items.blocked.size() != 1) {
            return fail("full refresh lost a blocked deletion snapshot");
        }
        if (expired_cursor && (graph.delta_requests.size() != 2 ||
                               graph.delta_requests.front() !=
                                   std::optional<std::string>{"expired"} ||
                               graph.delta_requests.back())) {
            return fail("blocked deletion did not exercise cursor expiry");
        }
        graph.reject_saved_cursor = false;
        if (synchronize() != exit_code::blocked || graph.upload_count != 0 ||
            items.blocked.size() != 1 || !items.find("me", "file")) {
            return fail(
                "blocked deletion was cleared and resurrected remotely"
            );
        }
        std::filesystem::remove(root / "file.txt");
        if (synchronize() != exit_code::success || items.find("me", "file") ||
            !items.blocked.empty() || graph.upload_count != 0) {
            return fail("resolved blocked deletion did not clear its snapshot");
        }
    }
    return EXIT_SUCCESS;
}

int test_local_delete_large_guard() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
        guarded_items.blocked.size() != 1 ||
        guarded_items.blocked[0].reason_code != "upload_delete_limit" ||
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
                .synchronize() != exit_code::success ||
        guarded_dry_items.blocked.size() != 1 ||
        guarded_dry_items.blocked[0].reason_code != "upload_delete_limit" ||
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
                .synchronize() != exit_code::success ||
        guarded_graph.deleted_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"guard-directory", "etag"},
            } ||
        !guarded_items.items.empty() || !guarded_metrics.last_success ||
        !guarded_items.blocked.empty() ||
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
            pending_guard_metrics,
            &default_console
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
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::delete_cases
