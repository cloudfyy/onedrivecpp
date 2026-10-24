#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace onedrive::test::sync::delete_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_local_delete_propagation() {
    const onedrive::cli::Console default_console;
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
    static_cast<void>(onedrive::sync::SyncEngine{
        config, graph, items, metrics, &default_console
    }
                          .synchronize());
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
    return EXIT_SUCCESS;
}

int test_local_delete_preview() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
        dry_config, dry_graph, dry_items, dry_metrics, &default_console
    }
                          .synchronize());
    if (!dry_graph.deleted_items.empty() ||
        !dry_items.pending_deletes_by_id.empty() ||
        !dry_items.find("me", "dry-removed")) {
        return fail("deletion dry run changed remote or local state");
    }
    return EXIT_SUCCESS;
}

int test_local_delete_selective_boundary() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
        onedrive::sync::engine_detail::sync_scope_fingerprint(
            onedrive::sync::detail::SyncList::load(
                sync_list,
                onedrive::sync::detail::root_file_policy(
                    selective_config.sync_root_files
                )
            )
                .fingerprint()
        );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config,
        selective_graph,
        selective_items,
        selective_metrics,
        &default_console
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
    return EXIT_SUCCESS;
}

int test_local_delete_filter_policy() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
        "hidden-delete", tracked_item(policy_root, "hidden-delete", ".hidden")
    );
    auto large_item = tracked_item(policy_root, "large-delete", "large.bin");
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
        onedrive::sync::engine_detail::sync_scope_fingerprint(
            onedrive::sync::detail::SyncList::configured(
                {
                    .sync_root = policy_root,
                    .include_root_files = policy_config.sync_root_files,
                    .nosync_enabled = policy_config.nosync_enabled,
                    .dotfiles = policy_config.dotfiles,
                    .maximum_file_size_bytes =
                        policy_config.maximum_file_size_bytes,
                }
            )
                .fingerprint()
        );
    static_cast<void>(onedrive::sync::SyncEngine{
        policy_config,
        policy_graph,
        policy_items,
        policy_metrics,
        &default_console
    }
                          .synchronize());
    if (!policy_graph.deleted_items.empty() || policy_items.size() != 3) {
        return fail("filtered local absence propagated remote deletions");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::delete_cases
