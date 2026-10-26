#include "support.hpp"
#include "move_cases.hpp"
#include "onedrive/exit_code.hpp"
#include <algorithm>

namespace onedrive::test::sync::move_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_local_move_rename_and_preview() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto root = temporary.path() / "local-moves";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "old.txt"};
        output << "data";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "moved-file", tracked_item(root, "moved-file", "old.txt")
    );
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        config, graph, items, metrics, &default_console
    }
                          .synchronize());
    const auto before = items.find("me", "moved-file");
    if (!before || before->local_device == 0 || before->local_inode == 0) {
        return fail("local move baseline identity was not persisted");
    }

    std::filesystem::rename(root / "old.txt", root / "renamed.txt");
    static_cast<void>(onedrive::sync::SyncEngine{
        config, graph, items, metrics, &default_console
    }
                          .synchronize());
    const auto moved = items.find("me", "moved-file");
    if (graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"moved-file", "renamed.txt"},
            } ||
        !graph.deleted_items.empty() || graph.upload_count != 0 || !moved ||
        moved->remote_path != "renamed.txt" ||
        moved->local_path != root / "renamed.txt" ||
        moved->local_device != before->local_device ||
        moved->local_inode != before->local_inode ||
        !items.pending_remote_moves_by_id.empty()) {
        return fail("local file rename was not applied as a remote move");
    }

    std::filesystem::rename(root / "renamed.txt", root / "dry-run.txt");
    auto dry_config = config;
    dry_config.dry_run = true;
    static_cast<void>(onedrive::sync::SyncEngine{
        dry_config, graph, items, metrics, &default_console
    }
                          .synchronize());
    if (graph.moved_remote_items.size() != 1 ||
        !items.pending_remote_moves_by_id.empty() ||
        items.find("me", "moved-file")->remote_path != "renamed.txt") {
        return fail("local move dry run changed remote or persisted state");
    }

    {
        std::ofstream output{root / "dry-run.txt", std::ios::app};
        output << "-changed";
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        config, graph, items, metrics, &default_console
    }
                          .synchronize());
    if (graph.moved_remote_items.size() != 2 ||
        graph.moved_remote_items.back() !=
            std::pair<std::string, std::string>{
                "moved-file",
                "dry-run.txt",
            } ||
        graph.upload_count != 1 ||
        graph.uploaded_paths != std::vector<std::string>{"dry-run.txt"}) {
        return fail("moved and modified file was not uploaded after move");
    }
    return EXIT_SUCCESS;
}

int test_local_move_new_parent() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto new_parent_root = temporary.path() / "move-new-parent";
    std::filesystem::create_directories(new_parent_root);
    {
        std::ofstream output{new_parent_root / "before.txt"};
        output << "data";
    }
    FakeItemStore new_parent_items;
    new_parent_items.saved_delta_link = "saved";
    new_parent_items.items.emplace(
        "new-parent-file",
        tracked_item(new_parent_root, "new-parent-file", "before.txt")
    );
    FakeGraphClient new_parent_graph;
    FakeMetrics new_parent_metrics;
    auto new_parent_config = config_for(new_parent_root, false);
    new_parent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::create_directories(new_parent_root / "New" / "Nested");
    std::filesystem::rename(
        new_parent_root / "before.txt",
        new_parent_root / "New" / "Nested" / "after.txt"
    );
    {
        std::ofstream output{
            new_parent_root / "New" / "Nested" / "after.txt", std::ios::app
        };
        output << "-changed";
    }
    auto new_parent_dry_config = new_parent_config;
    new_parent_dry_config.dry_run = true;
    std::ostringstream new_parent_dry_output;
    std::ostringstream new_parent_dry_error;
    const onedrive::cli::Console new_parent_dry_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
        },
        new_parent_dry_output,
        new_parent_dry_error
    };
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_dry_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics,
        &new_parent_dry_console
    }
                          .synchronize());
    const auto new_parent_before_dry_move =
        new_parent_items.find("me", "new-parent-file");
    if (!new_parent_graph.remote_mutations.empty() ||
        new_parent_graph.upload_count != 0 ||
        !new_parent_items.pending_uploads_by_path.empty() ||
        !new_parent_items.pending_remote_moves_by_id.empty() ||
        !new_parent_before_dry_move ||
        new_parent_before_dry_move->remote_path != "before.txt" ||
        !new_parent_dry_output.str().contains(R"("create_directories":"2")") ||
        !new_parent_dry_output.str().contains(R"("move_remote_items":"1")") ||
        !new_parent_dry_error.str().empty()) {
        return fail(
            "new-parent move dry run changed state or reported the wrong plan"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        new_parent_config,
        new_parent_graph,
        new_parent_items,
        new_parent_metrics,
        &default_console
    }
                          .synchronize());
    const auto new_parent_file = new_parent_items.find("me", "new-parent-file");
    if (new_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "mkdir:New",
                "mkdir:New/Nested",
                "move:new-parent-file:New/Nested/after.txt",
            } ||
        new_parent_graph.uploaded_paths !=
            std::vector<std::string>{"New/Nested/after.txt"} ||
        !new_parent_file ||
        new_parent_file->remote_path != "New/Nested/after.txt" ||
        new_parent_file->local_path !=
            new_parent_root / "New" / "Nested" / "after.txt") {
        return fail("file move into new parent was not ordered before upload");
    }
    return EXIT_SUCCESS;
}

int test_local_move_tracked_parent() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto tracked_parent_root = temporary.path() / "move-tracked-parent";
    std::filesystem::create_directories(tracked_parent_root / "Existing");
    {
        std::ofstream output{tracked_parent_root / "before.txt"};
        output << "data";
    }
    FakeItemStore tracked_parent_items;
    tracked_parent_items.saved_delta_link = "saved";
    tracked_parent_items.items.emplace(
        "existing-parent",
        tracked_item(tracked_parent_root, "existing-parent", "Existing", true)
    );
    tracked_parent_items.items.emplace(
        "tracked-parent-file",
        tracked_item(tracked_parent_root, "tracked-parent-file", "before.txt")
    );
    FakeGraphClient tracked_parent_graph;
    FakeMetrics tracked_parent_metrics;
    auto tracked_parent_config = config_for(tracked_parent_root, false);
    tracked_parent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        tracked_parent_config,
        tracked_parent_graph,
        tracked_parent_items,
        tracked_parent_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(
        tracked_parent_root / "before.txt",
        tracked_parent_root / "Existing" / "after.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        tracked_parent_config,
        tracked_parent_graph,
        tracked_parent_items,
        tracked_parent_metrics,
        &default_console
    }
                          .synchronize());
    if (tracked_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "move:tracked-parent-file:Existing/after.txt",
            } ||
        tracked_parent_graph.directory_create_count != 0) {
        return fail("move into tracked parent created a duplicate directory");
    }
    return EXIT_SUCCESS;
}

int test_local_move_directory_new_parent() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto new_directory_parent_root =
        temporary.path() / "directory-move-new-parent";
    std::filesystem::create_directories(new_directory_parent_root / "Old");
    {
        std::ofstream output{new_directory_parent_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeItemStore new_directory_parent_items;
    new_directory_parent_items.saved_delta_link = "saved";
    new_directory_parent_items.items.emplace(
        "new-parent-directory",
        tracked_item(
            new_directory_parent_root, "new-parent-directory", "Old", true
        )
    );
    auto new_parent_child = tracked_item(
        new_directory_parent_root, "new-parent-child", "Old/child.txt"
    );
    new_parent_child.parent_id = "new-parent-directory";
    new_directory_parent_items.items.emplace(
        "new-parent-child", new_parent_child
    );
    FakeGraphClient new_directory_parent_graph;
    new_directory_parent_graph.moved_item_directory = true;
    FakeMetrics new_directory_parent_metrics;
    auto new_directory_parent_config =
        config_for(new_directory_parent_root, false);
    new_directory_parent_config.sync_mode =
        onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        new_directory_parent_config,
        new_directory_parent_graph,
        new_directory_parent_items,
        new_directory_parent_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::create_directories(
        new_directory_parent_root / "New" / "Nested"
    );
    std::filesystem::rename(
        new_directory_parent_root / "Old",
        new_directory_parent_root / "New" / "Nested" / "Old"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        new_directory_parent_config,
        new_directory_parent_graph,
        new_directory_parent_items,
        new_directory_parent_metrics,
        &default_console
    }
                          .synchronize());
    const auto new_parent_moved_child =
        new_directory_parent_items.find("me", "new-parent-child");
    if (new_directory_parent_graph.remote_mutations !=
            std::vector<std::string>{
                "mkdir:New",
                "mkdir:New/Nested",
                "move:new-parent-directory:New/Nested/Old",
            } ||
        !new_parent_moved_child ||
        new_parent_moved_child->remote_path != "New/Nested/Old/child.txt" ||
        new_parent_moved_child->local_path != new_directory_parent_root /
                                                  "New" / "Nested" / "Old" /
                                                  "child.txt" ||
        new_directory_parent_graph.upload_count != 0) {
        return fail("directory move into new parent did not remap descendants");
    }
    return EXIT_SUCCESS;
}

int test_local_move_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto recovery_root = temporary.path() / "move-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "before.txt"};
        output << "data";
    }
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.items.emplace(
        "recovery-move",
        tracked_item(recovery_root, "recovery-move", "before.txt")
    );
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(
        recovery_root / "before.txt", recovery_root / "after.txt"
    );
    recovery_items.fail_commit_remote_move = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics,
            &default_console
        }
                              .synchronize());
        return fail("remote move commit failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_remote_moves_by_id.size() != 1) {
        return fail("remote move commit failure lost its journal");
    }
    recovery_items.fail_commit_remote_move = false;
    recovery_graph.move_conflict = true;
    recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "recovery-move",
        .name = "after.txt",
        .etag = "recovered-etag",
        .parent_id = "root",
        .remote_path = "after.txt",
        .last_modified = "2026-10-05T02:00:00Z",
    };
    static_cast<void>(onedrive::sync::SyncEngine{
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics,
        &default_console
    }
                          .synchronize());
    const auto recovered = recovery_items.find("me", "recovery-move");
    if (!recovered || recovered->remote_path != "after.txt" ||
        !recovery_items.pending_remote_moves_by_id.empty()) {
        return fail("pending remote move was not recovered");
    }
    return EXIT_SUCCESS;
}

int test_local_move_directory() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto directory_root = temporary.path() / "directory-move";
    std::filesystem::create_directories(directory_root / "Old");
    {
        std::ofstream output{directory_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeItemStore directory_items;
    directory_items.saved_delta_link = "saved";
    directory_items.items.emplace(
        "move-directory",
        tracked_item(directory_root, "move-directory", "Old", true)
    );
    directory_items.items.emplace(
        "move-directory-child",
        tracked_item(directory_root, "move-directory-child", "Old/child.txt")
    );
    FakeGraphClient directory_graph;
    directory_graph.moved_item_directory = true;
    FakeMetrics directory_metrics;
    auto directory_config = config_for(directory_root, false);
    directory_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        directory_config,
        directory_graph,
        directory_items,
        directory_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(directory_root / "Old", directory_root / "New");
    static_cast<void>(onedrive::sync::SyncEngine{
        directory_config,
        directory_graph,
        directory_items,
        directory_metrics,
        &default_console
    }
                          .synchronize());
    const auto moved_directory = directory_items.find("me", "move-directory");
    const auto moved_child = directory_items.find("me", "move-directory-child");
    if (directory_graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"move-directory", "New"},
            } ||
        !directory_graph.deleted_items.empty() || !moved_directory ||
        moved_directory->remote_path != "New" || !moved_child ||
        moved_child->remote_path != "New/child.txt" ||
        moved_child->local_path != directory_root / "New" / "child.txt") {
        return fail("local directory rename did not remap remote descendants");
    }
    return EXIT_SUCCESS;
}

int test_local_move_selective_boundaries() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto selective_root = temporary.path() / "selective-move";
    std::filesystem::create_directories(selective_root / "Included");
    std::filesystem::create_directories(selective_root / "Excluded");
    {
        std::ofstream output{selective_root / "Included" / "retained.txt"};
        output << "data";
    }
    const auto sync_list = temporary.path() / "move-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    selective_items.items.emplace(
        "selective-move",
        tracked_item(selective_root, "selective-move", "Included/retained.txt")
    );
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
    std::filesystem::rename(
        selective_root / "Included" / "retained.txt",
        selective_root / "Excluded" / "retained.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_config,
        selective_graph,
        selective_items,
        selective_metrics,
        &default_console
    }
                          .synchronize());
    if (!selective_graph.moved_remote_items.empty() ||
        !selective_graph.deleted_items.empty() ||
        selective_graph.upload_count != 0 ||
        !selective_items.find("me", "selective-move")) {
        return fail("selective sync boundary move changed remote state");
    }

    const auto selective_parent_root =
        temporary.path() / "selective-new-parent-move";
    std::filesystem::create_directories(selective_parent_root / "Included");
    {
        std::ofstream output{selective_parent_root / "Included" / "before.txt"};
        output << "data";
    }
    FakeItemStore selective_parent_items;
    selective_parent_items.saved_delta_link = "saved";
    selective_parent_items.items.emplace(
        "selective-parent-move",
        tracked_item(
            selective_parent_root,
            "selective-parent-move",
            "Included/before.txt"
        )
    );
    FakeGraphClient selective_parent_graph;
    FakeMetrics selective_parent_metrics;
    auto selective_parent_config = config_for(selective_parent_root, false);
    selective_parent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    selective_parent_config.sync_list = sync_list;
    selective_parent_items.saved_sync_filter_fingerprint =
        onedrive::sync::engine_detail::sync_scope_fingerprint(
            onedrive::sync::detail::SyncList::load(
                sync_list,
                onedrive::sync::detail::root_file_policy(
                    selective_parent_config.sync_root_files
                )
            )
                .fingerprint()
        );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_parent_config,
        selective_parent_graph,
        selective_parent_items,
        selective_parent_metrics,
        &default_console
    }
                          .synchronize());
    selective_parent_graph.remote_mutations.clear();
    std::filesystem::create_directories(
        selective_parent_root / "Included" / "New" / "Nested"
    );
    std::filesystem::rename(
        selective_parent_root / "Included" / "before.txt",
        selective_parent_root / "Included" / "New" / "Nested" / "after.txt"
    );
    static_cast<void>(onedrive::sync::SyncEngine{
        selective_parent_config,
        selective_parent_graph,
        selective_parent_items,
        selective_parent_metrics,
        &default_console
    }
                          .synchronize());
    if (selective_parent_graph.remote_mutations !=
        std::vector<std::string>{
            "mkdir:Included/New",
            "mkdir:Included/New/Nested",
            "move:selective-parent-move:"
            "Included/New/Nested/after.txt",
        }) {
        return fail("selective sync did not create included move parents");
    }
    return EXIT_SUCCESS;
}

int test_local_move_parent_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto parent_recovery_root =
        temporary.path() / "move-new-parent-recovery";
    std::filesystem::create_directories(parent_recovery_root);
    {
        std::ofstream output{parent_recovery_root / "before.txt"};
        output << "data";
    }
    FakeItemStore parent_recovery_items;
    parent_recovery_items.saved_delta_link = "saved";
    parent_recovery_items.items.emplace(
        "parent-recovery-move",
        tracked_item(parent_recovery_root, "parent-recovery-move", "before.txt")
    );
    FakeGraphClient parent_recovery_graph;
    FakeMetrics parent_recovery_metrics;
    auto parent_recovery_config = config_for(parent_recovery_root, false);
    parent_recovery_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        parent_recovery_config,
        parent_recovery_graph,
        parent_recovery_items,
        parent_recovery_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::create_directories(
        parent_recovery_root / "New" / "Nested"
    );
    std::filesystem::rename(
        parent_recovery_root / "before.txt",
        parent_recovery_root / "New" / "Nested" / "after.txt"
    );
    parent_recovery_graph.move_conflict = true;
    parent_recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "conflicting-item",
        .name = "after.txt",
        .etag = "conflicting-etag",
        .parent_id = "directory-2",
        .remote_path = "New/Nested/after.txt",
    };
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            parent_recovery_config,
            parent_recovery_graph,
            parent_recovery_items,
            parent_recovery_metrics,
            &default_console
        }
                              .synchronize());
        return fail("new-parent move conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (parent_recovery_graph.created_directory_paths !=
            std::vector<std::string>{"New", "New/Nested"} ||
        parent_recovery_items.pending_uploads_by_path.size() != 0 ||
        !parent_recovery_items.find("me", "directory-1") ||
        !parent_recovery_items.find("me", "directory-2")) {
        return fail("new-parent move conflict lost created parent state");
    }
    parent_recovery_graph.move_conflict = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        parent_recovery_config,
        parent_recovery_graph,
        parent_recovery_items,
        parent_recovery_metrics,
        &default_console
    }
                          .synchronize());
    const auto parent_recovered =
        parent_recovery_items.find("me", "parent-recovery-move");
    if (parent_recovery_graph.directory_create_count != 2 ||
        parent_recovery_graph.moved_remote_items.size() != 2 ||
        !parent_recovered ||
        parent_recovered->remote_path != "New/Nested/after.txt" ||
        !parent_recovery_items.pending_remote_moves_by_id.empty()) {
        return fail(
            "new-parent move did not recover without recreating parents"
        );
    }
    return EXIT_SUCCESS;
}

namespace {

int verify_local_move_permission(
    const std::string& drive_id, std::string_view label
) {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto permission_move_root =
        temporary.path() / ("move-permission-" + drive_id);
    std::filesystem::create_directories(permission_move_root);
    {
        std::ofstream output{permission_move_root / "before.txt"};
        output << "data";
    }
    FakeItemStore permission_move_items;
    permission_move_items.saved_delta_link = "saved";
    auto move_item =
        tracked_item(permission_move_root, "permission-move", "before.txt");
    move_item.drive_id = std::string{drive_id};
    permission_move_items.items.emplace("permission-move", std::move(move_item));
    FakeGraphClient permission_move_graph;
    FakeMetrics permission_move_metrics;
    auto permission_move_config = config_for(permission_move_root, false);
    permission_move_config.drive_id = std::string{drive_id};
    permission_move_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        permission_move_config,
        permission_move_graph,
        permission_move_items,
        permission_move_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(
        permission_move_root / "before.txt", permission_move_root / "after.txt"
    );
    permission_move_graph.move_permission_denied_id = "permission-move";
    if (onedrive::sync::SyncEngine{
            permission_move_config,
            permission_move_graph,
            permission_move_items,
            permission_move_metrics,
            &default_console
        }
                .synchronize() != onedrive::exit_code::blocked ||
        !permission_move_metrics.last_success ||
        !permission_move_items.pending_remote_moves_by_id.empty() ||
        !permission_move_graph.deleted_items.empty() ||
        permission_move_graph.upload_count != 0 ||
        permission_move_items.blocked.size() != 1 ||
        permission_move_items.blocked.front().drive_id != drive_id ||
        permission_move_items.blocked.front().remote_path != "after.txt" ||
        permission_move_items.blocked.front().reason_code !=
            "upload_remote_permission") {
        return fail(
            std::string{label} +
            " remote move permission failure deleted source or uploaded "
            "duplicate destination"
        );
    }
    if (onedrive::sync::SyncEngine{
            permission_move_config,
            permission_move_graph,
            permission_move_items,
            permission_move_metrics,
            &default_console
        }
                .synchronize() != onedrive::exit_code::blocked ||
        !permission_move_graph.deleted_items.empty() ||
        permission_move_graph.upload_count != 0 ||
        permission_move_items.blocked.size() != 1) {
        return fail(
            std::string{label} +
            " subsequent sync round did not protect blocked local move"
        );
    }
    std::filesystem::rename(
        permission_move_root / "after.txt", permission_move_root / "before.txt"
    );
    if (onedrive::sync::SyncEngine{
            permission_move_config,
            permission_move_graph,
            permission_move_items,
            permission_move_metrics,
            &default_console
        }
                .synchronize() != onedrive::exit_code::success ||
        !permission_move_items.blocked.empty()) {
        return fail(
            std::string{label} +
            " reverting blocked local move did not clear its blocked item"
        );
    }

    const auto journal_root =
        temporary.path() / ("move-permission-journal-" + std::string{drive_id});
    std::filesystem::create_directories(journal_root);
    {
        std::ofstream output{journal_root / "before.txt"};
        output << "data";
    }
    FakeItemStore journal_items;
    journal_items.saved_delta_link = "saved";
    auto journal_item =
        tracked_item(journal_root, "journal-move", "before.txt");
    journal_item.drive_id = std::string{drive_id};
    journal_items.items.emplace("journal-move", journal_item);
    FakeGraphClient journal_graph;
    FakeMetrics journal_metrics;
    auto journal_config = config_for(journal_root, false);
    journal_config.drive_id = std::string{drive_id};
    journal_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_config,
        journal_graph,
        journal_items,
        journal_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(
        journal_root / "before.txt", journal_root / "after.txt"
    );
    const auto tracked_before = journal_items.find(drive_id, "journal-move");
    if (!tracked_before) {
        return fail("missing tracked item before move journal test");
    }
    journal_items.pending_remote_moves_by_id.emplace(
        "journal-move",
        onedrive::storage::PendingRemoteMove{
            .drive_id = std::string{drive_id},
            .remote_id = "journal-move",
            .expected_etag = tracked_before->etag,
            .source_remote_path = "before.txt",
            .destination_remote_path = "after.txt",
            .destination_local_path = journal_root / "after.txt",
            .local_device = tracked_before->local_device,
            .local_inode = tracked_before->local_inode,
        }
    );
    journal_graph.move_permission_denied_id = "journal-move";
    std::ostringstream journal_output;
    std::ostringstream journal_errors;
    const onedrive::cli::Console journal_console{
        {.color = onedrive::cli::ColorMode::never,
         .output = onedrive::cli::OutputMode::json},
        journal_output,
        journal_errors,
    };
    if (onedrive::sync::SyncEngine{
            journal_config,
            journal_graph,
            journal_items,
            journal_metrics,
            &journal_console
        }
                .synchronize() != onedrive::exit_code::blocked ||
        !journal_metrics.last_success ||
        !journal_items.pending_remote_moves_by_id.empty() ||
        !journal_graph.deleted_items.empty() ||
        journal_graph.upload_count != 0 ||
        journal_items.blocked.size() != 1 ||
        journal_items.blocked.front().reason_code !=
            "upload_remote_permission" ||
        !(journal_output.str() + journal_errors.str())
             .contains("pending_remote_move_resource_blocked")) {
        return fail(
            std::string{label} +
            " pending remote move journal recovery did not block permission "
            "error"
        );
    }
    journal_graph.move_permission_denied_id.clear();
    if (onedrive::sync::SyncEngine{
            journal_config,
            journal_graph,
            journal_items,
            journal_metrics,
            &default_console
        }
                .synchronize() != onedrive::exit_code::success ||
        !journal_items.blocked.empty() ||
        !journal_items.find(drive_id, "journal-move") ||
        journal_items.find(drive_id, "journal-move")->remote_path !=
            "after.txt") {
        return fail(
            std::string{label} +
            " pending remote move permission block did not recover after grant"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int test_business_local_move_permission() {
    return verify_local_move_permission(
        "business-drive", "OneDrive for Business"
    );
}

int test_sharepoint_local_move_permission() {
    return verify_local_move_permission("b!sp-library", "SharePoint");
}

} // namespace onedrive::test::sync::move_cases
