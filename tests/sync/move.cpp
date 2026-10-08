#include "support.hpp"

#include <algorithm>

namespace {

using namespace onedrive::test::sync;

int test_remote_moves() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto renamed_root = temporary.path() / "renamed";
    std::filesystem::create_directories(renamed_root);
    {
        std::ofstream output{renamed_root / "old.txt"};
        output << "data";
    }
    FakeGraphClient renamed_graph;
    renamed_graph.changes = {file("renamed", "new.txt", 4)};
    FakeItemStore renamed_items;
    renamed_items.saved_delta_link = "saved";
    renamed_items.items.emplace(
        "renamed", tracked_item(renamed_root, "renamed", "old.txt")
    );
    FakeMetrics renamed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(renamed_root, false),
            renamed_graph,
            renamed_items,
            renamed_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(renamed_root / "old.txt") ||
        !std::filesystem::exists(renamed_root / "new.txt") ||
        renamed_graph.download_count != 0 ||
        renamed_items.applied_delta.upserts.size() != 1 ||
        renamed_items.applied_delta.upserts[0].remote_path != "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_path !=
            renamed_root / "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_size != 4 ||
        !renamed_items.pending_moves_by_id.empty() ||
        !renamed_metrics.last_success) {
        return fail("remote file rename was not applied locally");
    }

    const auto journal_failure_root = temporary.path() / "move-journal-failure";
    std::filesystem::create_directories(journal_failure_root);
    {
        std::ofstream output{journal_failure_root / "before.txt"};
        output << "data";
    }
    FakeItemStore journal_failure_items;
    journal_failure_items.saved_delta_link = "saved";
    journal_failure_items.items.emplace(
        "journal-failure-move",
        tracked_item(journal_failure_root, "journal-failure-move", "before.txt")
    );
    FakeGraphClient journal_failure_graph;
    FakeMetrics journal_failure_metrics;
    auto journal_failure_config = config_for(journal_failure_root, false);
    journal_failure_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_failure_config,
        journal_failure_graph,
        journal_failure_items,
        journal_failure_metrics,
        &default_console
    }
                          .synchronize());
    std::filesystem::rename(
        journal_failure_root / "before.txt", journal_failure_root / "after.txt"
    );
    journal_failure_items.fail_pending_remote_move_save = true;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            journal_failure_config,
            journal_failure_graph,
            journal_failure_items,
            journal_failure_metrics,
            &default_console
        }
                              .synchronize());
        return fail("remote move journal failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!journal_failure_graph.moved_remote_items.empty() ||
        !journal_failure_items.pending_remote_moves_by_id.empty()) {
        return fail("unjournaled remote move reached Microsoft Graph");
    }
    journal_failure_items.fail_pending_remote_move_save = false;
    static_cast<void>(onedrive::sync::SyncEngine{
        journal_failure_config,
        journal_failure_graph,
        journal_failure_items,
        journal_failure_metrics,
        &default_console
    }
                          .synchronize());
    if (journal_failure_graph.moved_remote_items !=
            std::vector<std::pair<std::string, std::string>>{
                {"journal-failure-move", "after.txt"},
            } ||
        !journal_failure_items.pending_remote_moves_by_id.empty()) {
        return fail("remote move did not recover after journal failure");
    }

    const auto recovery_root = temporary.path() / "move-recovery";
    std::filesystem::create_directories(recovery_root);
    {
        std::ofstream output{recovery_root / "old.txt"};
        output << "data";
    }
    FakeGraphClient failed_move_graph;
    failed_move_graph.changes = {
        file("move-recovery", "new.txt", 4),
    };
    FakeItemStore failed_move_items;
    failed_move_items.saved_delta_link = "saved";
    failed_move_items.items.emplace(
        "move-recovery", tracked_item(recovery_root, "move-recovery", "old.txt")
    );
    failed_move_items.fail_apply_delta = true;
    FakeMetrics failed_move_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            failed_move_graph,
            failed_move_items,
            failed_move_metrics,
            &default_console
        }
                              .synchronize());
        return fail("delta commit failure did not interrupt remote move");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(recovery_root / "old.txt") ||
        !std::filesystem::exists(recovery_root / "new.txt") ||
        failed_move_items.pending_moves_by_id.size() != 1 ||
        failed_move_metrics.last_success) {
        return fail("interrupted remote move did not retain its journal");
    }
    FakeGraphClient recovered_move_graph;
    recovered_move_graph.changes = {
        file("move-recovery", "new.txt", 4),
    };
    FakeItemStore recovered_move_items;
    recovered_move_items.saved_delta_link = "saved";
    recovered_move_items.items = failed_move_items.items;
    recovered_move_items.pending_moves_by_id =
        failed_move_items.pending_moves_by_id;
    FakeMetrics recovered_move_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            recovered_move_graph,
            recovered_move_items,
            recovered_move_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        recovered_move_graph.download_count != 0 ||
        !recovered_move_items.pending_moves_by_id.empty() ||
        !recovered_move_metrics.last_success) {
        return fail("pending remote move was not recovered by inode identity");
    }
    FakeGraphClient deleted_move_graph;
    deleted_move_graph.changes = {
        deleted_item("move-recovery"),
    };
    FakeItemStore deleted_move_items;
    deleted_move_items.saved_delta_link = "saved";
    deleted_move_items.items = failed_move_items.items;
    deleted_move_items.pending_moves_by_id =
        failed_move_items.pending_moves_by_id;
    FakeMetrics deleted_move_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(recovery_root, false),
            deleted_move_graph,
            deleted_move_items,
            deleted_move_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(recovery_root / "new.txt") ||
        !deleted_move_items.pending_moves_by_id.empty() ||
        deleted_move_items.applied_delta.removals !=
            std::vector<std::string>{"move-recovery"}) {
        return fail("remotely deleted pending move target was not removed");
    }

    const auto adopted_root = temporary.path() / "adopted";
    std::filesystem::create_directories(adopted_root);
    {
        std::ofstream output{adopted_root / "new.txt"};
        output << "data";
    }
    FakeGraphClient adopted_graph;
    adopted_graph.changes = {file("adopted", "new.txt", 4)};
    FakeItemStore adopted_items;
    adopted_items.saved_delta_link = "saved";
    auto adopted_state = tracked_item(adopted_root, "adopted", "new.txt");
    adopted_state.name = "old.txt";
    adopted_state.remote_path = "old.txt";
    adopted_state.local_path = adopted_root / "old.txt";
    adopted_items.items.emplace("adopted", std::move(adopted_state));
    FakeMetrics adopted_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(adopted_root, false),
            adopted_graph,
            adopted_items,
            adopted_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        adopted_graph.download_count != 0 ||
        adopted_items.applied_delta.upserts.size() != 1 ||
        adopted_items.applied_delta.upserts[0].local_path !=
            adopted_root / "new.txt") {
        return fail("interrupted remote move destination was not adopted");
    }

    const auto mismatched_root = temporary.path() / "mismatched-move";
    std::filesystem::create_directories(mismatched_root);
    {
        std::ofstream output{mismatched_root / "new.txt"};
        output << "data";
    }
    FakeGraphClient mismatched_graph;
    mismatched_graph.changes = {
        file("mismatched-move", "new.txt", 4),
    };
    FakeItemStore mismatched_items;
    mismatched_items.saved_delta_link = "saved";
    auto mismatched_state =
        tracked_item(mismatched_root, "mismatched-move", "new.txt");
    mismatched_state.name = "old.txt";
    mismatched_state.remote_path = "old.txt";
    mismatched_state.local_path = mismatched_root / "old.txt";
    mismatched_items.items.emplace(
        "mismatched-move", std::move(mismatched_state)
    );
    mismatched_items.pending_moves_by_id.emplace(
        "mismatched-move",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "mismatched-move",
            .source_path = mismatched_root / "old.txt",
            .destination_path = mismatched_root / "new.txt",
            .source_device = 0,
            .source_inode = 0,
        }
    );
    FakeMetrics mismatched_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(mismatched_root, false),
            mismatched_graph,
            mismatched_items,
            mismatched_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        mismatched_items.applied_delta.blocked_upserts.size() != 1 ||
        mismatched_items.applied_delta.blocked_upserts[0].reason_code !=
            "pending_move_conflict" ||
        mismatched_items.pending_moves_by_id.size() != 1) {
        return fail("mismatched pending move destination was adopted");
    }

    const auto changed_root = temporary.path() / "changed";
    std::filesystem::create_directories(changed_root);
    {
        std::ofstream output{changed_root / "old.txt"};
        output << "data";
    }
    auto changed = file("changed", "folder/new.txt", 4);
    changed.etag = "changed-etag";
    changed.last_modified = "2026-10-04T10:00:00Z";
    changed.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "c6c1c9a9c8543f1e4cd980064cf1625eeb61a90703b2464fff039f21682508b3",
    };
    FakeGraphClient changed_graph;
    changed_graph.changes = {changed};
    changed_graph.contents["changed"] = "next";
    FakeItemStore changed_items;
    changed_items.saved_delta_link = "saved";
    changed_items.items.emplace(
        "changed", tracked_item(changed_root, "changed", "old.txt")
    );
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(changed_root / "old.txt") ||
        changed_graph.download_count != 1) {
        return fail("moved file with changed content was not downloaded");
    }
    {
        std::ifstream input{changed_root / "folder" / "new.txt"};
        const std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (content != "next") {
            return fail("moved file did not receive changed remote content");
        }
    }

    const auto directory_root = temporary.path() / "directory";
    std::filesystem::create_directories(directory_root / "Old");
    {
        std::ofstream output{directory_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeGraphClient directory_graph;
    directory_graph.changes = {
        {
            .id = "directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
    };
    FakeItemStore directory_items;
    directory_items.saved_delta_link = "saved";
    directory_items.items.emplace(
        "directory", tracked_item(directory_root, "directory", "Old", true)
    );
    directory_items.items.emplace(
        "child", tracked_item(directory_root, "child", "Old/child.txt")
    );
    FakeMetrics directory_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_root, false),
            directory_graph,
            directory_items,
            directory_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(directory_root / "Old") ||
        !std::filesystem::exists(directory_root / "New" / "child.txt") ||
        directory_graph.download_count != 0 ||
        directory_items.applied_delta.upserts.size() != 2) {
        return fail("remote directory move did not move its tracked subtree");
    }
    const auto child_state = std::ranges::find(
        directory_items.applied_delta.upserts,
        "child",
        &onedrive::storage::ItemState::remote_id
    );
    if (child_state == directory_items.applied_delta.upserts.end() ||
        child_state->remote_path != "New/child.txt" ||
        child_state->local_path != directory_root / "New" / "child.txt") {
        return fail("remote directory move did not remap descendant state");
    }

    const auto ordered_root = temporary.path() / "ordered-moves";
    std::filesystem::create_directories(ordered_root);
    {
        std::ofstream first{ordered_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient ordered_graph;
    ordered_graph.changes = {
        file("first", "B.txt", 4),
        file("second", "C.txt", 4),
    };
    FakeItemStore ordered_items;
    ordered_items.saved_delta_link = "saved";
    ordered_items.items.emplace(
        "first", tracked_item(ordered_root, "first", "A.txt")
    );
    ordered_items.items.emplace(
        "second", tracked_item(ordered_root, "second", "B.txt")
    );
    FakeMetrics ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_root, false),
            ordered_graph,
            ordered_items,
            ordered_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(ordered_root / "A.txt") ||
        !std::filesystem::exists(ordered_root / "B.txt") ||
        !std::filesystem::exists(ordered_root / "C.txt") ||
        ordered_graph.download_count != 0 ||
        !ordered_items.applied_delta.blocked_upserts.empty()) {
        return fail("dependent remote move chain was not ordered safely");
    }
    {
        std::ifstream first{ordered_root / "B.txt"};
        std::ifstream second{ordered_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "aaaa" || second_content != "bbbb") {
            return fail("dependent remote move chain swapped local content");
        }
    }

    const auto ordered_recovery_root =
        temporary.path() / "ordered-move-recovery";
    std::filesystem::create_directories(ordered_recovery_root);
    {
        std::ofstream first{ordered_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_recovery_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient failed_ordered_graph;
    failed_ordered_graph.changes = {
        file("recovery-first", "B.txt", 4),
        file("recovery-second", "C.txt", 4),
    };
    FakeItemStore failed_ordered_items;
    failed_ordered_items.saved_delta_link = "saved";
    failed_ordered_items.items.emplace(
        "recovery-first",
        tracked_item(ordered_recovery_root, "recovery-first", "A.txt")
    );
    failed_ordered_items.items.emplace(
        "recovery-second",
        tracked_item(ordered_recovery_root, "recovery-second", "B.txt")
    );
    failed_ordered_items.fail_apply_delta = true;
    FakeMetrics failed_ordered_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            failed_ordered_graph,
            failed_ordered_items,
            failed_ordered_metrics,
            &default_console
        }
                              .synchronize());
        return fail("dependent move commit failure did not interrupt sync");
    } catch (const std::runtime_error&) {
    }
    if (failed_ordered_items.pending_moves_by_id.size() != 2) {
        return fail("dependent move failure did not preserve both journals");
    }
    FakeGraphClient recovered_ordered_graph;
    recovered_ordered_graph.changes = failed_ordered_graph.changes;
    FakeItemStore recovered_ordered_items;
    recovered_ordered_items.saved_delta_link = "saved";
    recovered_ordered_items.items = failed_ordered_items.items;
    recovered_ordered_items.pending_moves_by_id =
        failed_ordered_items.pending_moves_by_id;
    FakeMetrics recovered_ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            recovered_ordered_graph,
            recovered_ordered_items,
            recovered_ordered_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        recovered_ordered_graph.download_count != 0 ||
        !recovered_ordered_items.pending_moves_by_id.empty() ||
        !recovered_ordered_metrics.last_success) {
        return fail("dependent remote move journals were not recovered");
    }

    const auto nested_root = temporary.path() / "nested-moves";
    std::filesystem::create_directories(nested_root / "Old");
    {
        std::ofstream output{nested_root / "Old" / "before.txt"};
        output << "data";
    }
    FakeGraphClient nested_graph;
    nested_graph.changes = {
        {
            .id = "nested-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("nested-child", "New/after.txt", 4),
    };
    FakeItemStore nested_items;
    nested_items.saved_delta_link = "saved";
    nested_items.items.emplace(
        "nested-directory",
        tracked_item(nested_root, "nested-directory", "Old", true)
    );
    nested_items.items.emplace(
        "nested-child",
        tracked_item(nested_root, "nested-child", "Old/before.txt")
    );
    FakeMetrics nested_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nested_root, false),
            nested_graph,
            nested_items,
            nested_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(nested_root / "Old") ||
        std::filesystem::exists(nested_root / "New" / "before.txt") ||
        !std::filesystem::exists(nested_root / "New" / "after.txt") ||
        nested_graph.download_count != 0 ||
        !nested_items.applied_delta.blocked_upserts.empty()) {
        return fail(
            "child rename was not remapped after its parent directory move"
        );
    }

    const auto cycle_root = temporary.path() / "move-cycle";
    std::filesystem::create_directories(cycle_root);
    {
        std::ofstream first{cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{cycle_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient cycle_graph;
    cycle_graph.changes = {
        file("cycle-first", "B.txt", 4),
        file("cycle-second", "A.txt", 4),
    };
    FakeItemStore cycle_items;
    cycle_items.saved_delta_link = "saved";
    cycle_items.items.emplace(
        "cycle-first", tracked_item(cycle_root, "cycle-first", "A.txt")
    );
    cycle_items.items.emplace(
        "cycle-second", tracked_item(cycle_root, "cycle-second", "B.txt")
    );
    FakeMetrics cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cycle_root, false),
            cycle_graph,
            cycle_items,
            cycle_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !std::filesystem::exists(cycle_root / "A.txt") ||
        !std::filesystem::exists(cycle_root / "B.txt") ||
        !cycle_items.applied_delta.blocked_upserts.empty() ||
        !cycle_items.pending_moves_by_id.empty()) {
        return fail("remote move dependency cycle was not staged safely");
    }
    {
        std::ifstream first{cycle_root / "A.txt"};
        std::ifstream second{cycle_root / "B.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "bbbb" || second_content != "aaaa") {
            return fail("staged remote name exchange lost local content");
        }
    }
    if (std::ranges::any_of(
            std::filesystem::directory_iterator{cycle_root},
            [](const std::filesystem::directory_entry& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-move-"
                );
            }
        )) {
        return fail("successful name exchange retained its staging path");
    }

    const auto three_cycle_root = temporary.path() / "three-move-cycle";
    std::filesystem::create_directories(three_cycle_root);
    {
        std::ofstream first{three_cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{three_cycle_root / "B.txt"};
        second << "bbbb";
        std::ofstream third{three_cycle_root / "C.txt"};
        third << "cccc";
    }
    FakeGraphClient three_cycle_graph;
    three_cycle_graph.changes = {
        file("three-first", "B.txt", 4),
        file("three-second", "C.txt", 4),
        file("three-third", "A.txt", 4),
    };
    FakeItemStore three_cycle_items;
    three_cycle_items.saved_delta_link = "saved";
    three_cycle_items.items.emplace(
        "three-first", tracked_item(three_cycle_root, "three-first", "A.txt")
    );
    three_cycle_items.items.emplace(
        "three-second", tracked_item(three_cycle_root, "three-second", "B.txt")
    );
    three_cycle_items.items.emplace(
        "three-third", tracked_item(three_cycle_root, "three-third", "C.txt")
    );
    FakeMetrics three_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(three_cycle_root, false),
            three_cycle_graph,
            three_cycle_items,
            three_cycle_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !three_cycle_items.pending_moves_by_id.empty() ||
        !three_cycle_items.applied_delta.blocked_upserts.empty()) {
        return fail("three-item remote move cycle was not staged safely");
    }
    {
        std::ifstream first{three_cycle_root / "A.txt"};
        std::ifstream second{three_cycle_root / "B.txt"};
        std::ifstream third{three_cycle_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        const std::string third_content{
            std::istreambuf_iterator<char>{third},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "cccc" || second_content != "aaaa" ||
            third_content != "bbbb") {
            return fail("three-item staged move cycle lost local content");
        }
    }

    const auto directory_cycle_root = temporary.path() / "directory-move-cycle";
    std::filesystem::create_directories(directory_cycle_root / "A");
    std::filesystem::create_directories(directory_cycle_root / "B");
    {
        std::ofstream first{directory_cycle_root / "A" / "first.txt"};
        first << "aaaa";
        std::ofstream second{directory_cycle_root / "B" / "second.txt"};
        second << "bbbb";
    }
    FakeGraphClient directory_cycle_graph;
    directory_cycle_graph.changes = {
        {
            .id = "cycle-directory-first",
            .name = "B",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "B",
            .directory = true,
        },
        {
            .id = "cycle-directory-second",
            .name = "A",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "A",
            .directory = true,
        },
    };
    FakeItemStore directory_cycle_items;
    directory_cycle_items.saved_delta_link = "saved";
    directory_cycle_items.items.emplace(
        "cycle-directory-first",
        tracked_item(directory_cycle_root, "cycle-directory-first", "A", true)
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second",
        tracked_item(directory_cycle_root, "cycle-directory-second", "B", true)
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-first-child",
        tracked_item(
            directory_cycle_root, "cycle-directory-first-child", "A/first.txt"
        )
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second-child",
        tracked_item(
            directory_cycle_root, "cycle-directory-second-child", "B/second.txt"
        )
    );
    FakeMetrics directory_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_cycle_root, false),
            directory_cycle_graph,
            directory_cycle_items,
            directory_cycle_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !std::filesystem::exists(directory_cycle_root / "A" / "second.txt") ||
        !std::filesystem::exists(directory_cycle_root / "B" / "first.txt") ||
        !directory_cycle_items.pending_moves_by_id.empty() ||
        directory_cycle_items.applied_delta.upserts.size() != 4) {
        return fail("remote directory name exchange was not staged safely");
    }

    const auto journaled_staging_root =
        temporary.path() / "journaled-staging-move-cycle";
    std::filesystem::create_directories(journaled_staging_root);
    {
        std::ofstream first{journaled_staging_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{journaled_staging_root / "B.txt"};
        second << "bbbb";
    }
    const auto journaled_staging_path =
        journaled_staging_root / ".A.txt.onedrive-move-journaled";
    struct stat journaled_source_identity{};
    if (::stat(
            (journaled_staging_root / "A.txt").c_str(),
            &journaled_source_identity
        ) == -1) {
        return fail("cannot inspect journaled staging source identity");
    }
    FakeGraphClient journaled_staging_graph;
    journaled_staging_graph.changes = {
        file("journaled-staging-first", "B.txt", 4),
        file("journaled-staging-second", "A.txt", 4),
    };
    FakeItemStore journaled_staging_items;
    journaled_staging_items.saved_delta_link = "saved";
    journaled_staging_items.items.emplace(
        "journaled-staging-first",
        tracked_item(journaled_staging_root, "journaled-staging-first", "A.txt")
    );
    journaled_staging_items.items.emplace(
        "journaled-staging-second",
        tracked_item(
            journaled_staging_root, "journaled-staging-second", "B.txt"
        )
    );
    journaled_staging_items.pending_moves_by_id.emplace(
        "journaled-staging-first",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "journaled-staging-first",
            .source_path = journaled_staging_root / "A.txt",
            .destination_path = journaled_staging_root / "B.txt",
            .staging_path = journaled_staging_path,
            .source_device =
                static_cast<std::uint64_t>(journaled_source_identity.st_dev),
            .source_inode =
                static_cast<std::uint64_t>(journaled_source_identity.st_ino),
        }
    );
    FakeMetrics journaled_staging_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(journaled_staging_root, false),
            journaled_staging_graph,
            journaled_staging_items,
            journaled_staging_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(journaled_staging_path) ||
        !journaled_staging_items.pending_moves_by_id.empty() ||
        !journaled_staging_metrics.last_success) {
        return fail("journaled staging move was not recovered");
    }
    {
        std::ifstream first{journaled_staging_root / "A.txt"};
        std::ifstream second{journaled_staging_root / "B.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "bbbb" || second_content != "aaaa") {
            return fail("journaled staging recovery lost local content");
        }
    }

    const auto staged_recovery_root =
        temporary.path() / "partially-staged-move-cycle";
    std::filesystem::create_directories(staged_recovery_root);
    {
        std::ofstream first{staged_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{staged_recovery_root / "B.txt"};
        second << "bbbb";
    }
    const auto staged_path =
        staged_recovery_root / ".A.txt.onedrive-move-recovery";
    auto staged_first =
        tracked_item(staged_recovery_root, "staged-recovery-first", "A.txt");
    auto staged_second =
        tracked_item(staged_recovery_root, "staged-recovery-second", "B.txt");
    std::filesystem::rename(staged_recovery_root / "A.txt", staged_path);
    struct stat staged_identity{};
    if (::stat(staged_path.c_str(), &staged_identity) == -1) {
        return fail("cannot inspect staged recovery fixture identity");
    }
    FakeGraphClient staged_recovery_graph;
    staged_recovery_graph.changes = {
        file("staged-recovery-first", "B.txt", 4),
        file("staged-recovery-second", "A.txt", 4),
    };
    FakeItemStore staged_recovery_items;
    staged_recovery_items.saved_delta_link = "saved";
    staged_recovery_items.items.emplace(
        "staged-recovery-first", std::move(staged_first)
    );
    staged_recovery_items.items.emplace(
        "staged-recovery-second", std::move(staged_second)
    );
    staged_recovery_items.pending_moves_by_id.emplace(
        "staged-recovery-first",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "staged-recovery-first",
            .source_path = staged_recovery_root / "A.txt",
            .destination_path = staged_recovery_root / "B.txt",
            .staging_path = staged_path,
            .source_device = static_cast<std::uint64_t>(staged_identity.st_dev),
            .source_inode = static_cast<std::uint64_t>(staged_identity.st_ino),
        }
    );
    FakeMetrics staged_recovery_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(staged_recovery_root, false),
            staged_recovery_graph,
            staged_recovery_items,
            staged_recovery_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(staged_path) ||
        !staged_recovery_items.pending_moves_by_id.empty() ||
        !staged_recovery_metrics.last_success) {
        return fail("partially staged move cycle was not recovered");
    }

    const auto staged_deletion_root = temporary.path() / "staged-move-deletion";
    std::filesystem::create_directories(staged_deletion_root);
    const auto staged_deletion_path =
        staged_deletion_root / ".A.txt.onedrive-move-deletion";
    {
        std::ofstream output{staged_deletion_path};
        output << "data";
    }
    struct stat staged_deletion_identity{};
    if (::stat(staged_deletion_path.c_str(), &staged_deletion_identity) == -1) {
        return fail("cannot inspect staged deletion fixture identity");
    }
    FakeGraphClient staged_deletion_graph;
    staged_deletion_graph.changes = {
        deleted_item("staged-deletion"),
    };
    FakeItemStore staged_deletion_items;
    staged_deletion_items.saved_delta_link = "saved";
    auto staged_deletion_state = tracked_item(
        staged_deletion_root, "staged-deletion", ".A.txt.onedrive-move-deletion"
    );
    staged_deletion_state.name = "A.txt";
    staged_deletion_state.remote_path = "A.txt";
    staged_deletion_state.local_path = staged_deletion_root / "A.txt";
    staged_deletion_items.items.emplace(
        "staged-deletion", std::move(staged_deletion_state)
    );
    staged_deletion_items.pending_moves_by_id.emplace(
        "staged-deletion",
        onedrive::storage::PendingMove{
            .drive_id = "me",
            .remote_id = "staged-deletion",
            .source_path = staged_deletion_root / "A.txt",
            .destination_path = staged_deletion_root / "B.txt",
            .staging_path = staged_deletion_path,
            .source_device =
                static_cast<std::uint64_t>(staged_deletion_identity.st_dev),
            .source_inode =
                static_cast<std::uint64_t>(staged_deletion_identity.st_ino),
        }
    );
    FakeMetrics staged_deletion_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(staged_deletion_root, false),
            staged_deletion_graph,
            staged_deletion_items,
            staged_deletion_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        std::filesystem::exists(staged_deletion_path) ||
        !staged_deletion_items.pending_moves_by_id.empty() ||
        staged_deletion_items.applied_delta.removals !=
            std::vector<std::string>{"staged-deletion"}) {
        return fail("remotely deleted staged move was not removed");
    }

    const auto cycle_recovery_root = temporary.path() / "move-cycle-recovery";
    std::filesystem::create_directories(cycle_recovery_root);
    {
        std::ofstream first{cycle_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{cycle_recovery_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient failed_cycle_graph;
    failed_cycle_graph.changes = {
        file("recovery-cycle-first", "B.txt", 4),
        file("recovery-cycle-second", "A.txt", 4),
    };
    FakeItemStore failed_cycle_items;
    failed_cycle_items.saved_delta_link = "saved";
    failed_cycle_items.items.emplace(
        "recovery-cycle-first",
        tracked_item(cycle_recovery_root, "recovery-cycle-first", "A.txt")
    );
    failed_cycle_items.items.emplace(
        "recovery-cycle-second",
        tracked_item(cycle_recovery_root, "recovery-cycle-second", "B.txt")
    );
    failed_cycle_items.fail_apply_delta = true;
    FakeMetrics failed_cycle_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(cycle_recovery_root, false),
            failed_cycle_graph,
            failed_cycle_items,
            failed_cycle_metrics,
            &default_console
        }
                              .synchronize());
        return fail("staged cycle commit failure did not interrupt sync");
    } catch (const std::runtime_error&) {
    }
    if (failed_cycle_items.pending_moves_by_id.size() != 2) {
        return fail("staged cycle did not retain its recovery journals");
    }
    FakeGraphClient recovered_cycle_graph;
    recovered_cycle_graph.changes = failed_cycle_graph.changes;
    FakeItemStore recovered_cycle_items;
    recovered_cycle_items.saved_delta_link = "saved";
    recovered_cycle_items.items = failed_cycle_items.items;
    recovered_cycle_items.pending_moves_by_id =
        failed_cycle_items.pending_moves_by_id;
    FakeMetrics recovered_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cycle_recovery_root, false),
            recovered_cycle_graph,
            recovered_cycle_items,
            recovered_cycle_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !recovered_cycle_items.pending_moves_by_id.empty() ||
        recovered_cycle_graph.download_count != 0 ||
        !recovered_cycle_metrics.last_success) {
        return fail("staged move cycle was not recovered after restart");
    }

    const auto blocked_dependency_root =
        temporary.path() / "blocked-move-dependency";
    std::filesystem::create_directories(blocked_dependency_root / "Old");
    std::filesystem::create_directories(blocked_dependency_root / "New");
    {
        std::ofstream output{blocked_dependency_root / "Old" / "before.txt"};
        output << "data";
    }
    FakeGraphClient blocked_dependency_graph;
    blocked_dependency_graph.changes = {
        {
            .id = "blocked-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("blocked-child", "New/after.txt", 4),
    };
    FakeItemStore blocked_dependency_items;
    blocked_dependency_items.saved_delta_link = "saved";
    blocked_dependency_items.items.emplace(
        "blocked-directory",
        tracked_item(blocked_dependency_root, "blocked-directory", "Old", true)
    );
    blocked_dependency_items.items.emplace(
        "blocked-child",
        tracked_item(blocked_dependency_root, "blocked-child", "Old/before.txt")
    );
    FakeMetrics blocked_dependency_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(blocked_dependency_root, false),
            blocked_dependency_graph,
            blocked_dependency_items,
            blocked_dependency_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        !std::filesystem::exists(
            blocked_dependency_root / "Old" / "before.txt"
        ) ||
        std::filesystem::exists(
            blocked_dependency_root / "New" / "after.txt"
        ) ||
        blocked_dependency_items.applied_delta.blocked_upserts.size() != 2 ||
        std::ranges::find(
            blocked_dependency_items.applied_delta.blocked_upserts,
            "move_dependency_blocked",
            &onedrive::storage::BlockedItem::reason_code
        ) == blocked_dependency_items.applied_delta.blocked_upserts.end()) {
        return fail("failed prerequisite did not block its dependent move");
    }

    const auto modified_root = temporary.path() / "modified-move";
    std::filesystem::create_directories(modified_root);
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "data";
    }
    FakeItemStore modified_items;
    modified_items.saved_delta_link = "saved";
    modified_items.items.emplace(
        "modified", tracked_item(modified_root, "modified", "old.txt")
    );
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "user data";
    }
    FakeGraphClient modified_graph;
    modified_graph.changes = {file("modified", "new.txt", 4)};
    FakeMetrics modified_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(modified_root, false),
            modified_graph,
            modified_items,
            modified_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        !std::filesystem::exists(modified_root / "old.txt") ||
        std::filesystem::exists(modified_root / "new.txt") ||
        modified_items.applied_delta.blocked_upserts.size() != 1 ||
        modified_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification") {
        return fail("remote move overwrote a locally modified source");
    }

    const auto collision_root = temporary.path() / "collision";
    std::filesystem::create_directories(collision_root);
    {
        std::ofstream source{collision_root / "old.txt"};
        source << "data";
        std::ofstream destination{collision_root / "new.txt"};
        destination << "local";
    }
    FakeItemStore collision_items;
    collision_items.saved_delta_link = "saved";
    collision_items.items.emplace(
        "collision", tracked_item(collision_root, "collision", "old.txt")
    );
    FakeGraphClient collision_graph;
    collision_graph.changes = {file("collision", "new.txt", 4)};
    FakeMetrics collision_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(collision_root, false),
            collision_graph,
            collision_items,
            collision_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        !std::filesystem::exists(collision_root / "old.txt") ||
        collision_items.applied_delta.blocked_upserts.size() != 1 ||
        collision_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move replaced an existing local destination");
    }

    const auto dry_root = temporary.path() / "dry-move";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "old.txt"};
        output << "data";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace("dry", tracked_item(dry_root, "dry", "old.txt"));
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("dry", "new.txt", 4)};
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true),
            dry_graph,
            dry_items,
            dry_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !std::filesystem::exists(dry_root / "old.txt") ||
        std::filesystem::exists(dry_root / "new.txt") ||
        dry_items.apply_count != 0) {
        return fail("remote move dry run changed local state");
    }

    const auto symlink_root = temporary.path() / "symlink-move";
    const auto outside = temporary.path() / "outside.txt";
    std::filesystem::create_directories(symlink_root);
    {
        std::ofstream output{outside};
        output << "data";
    }
    std::filesystem::create_symlink(outside, symlink_root / "old.txt");
    FakeItemStore symlink_items;
    symlink_items.saved_delta_link = "saved";
    auto symlink_state = tracked_item(symlink_root, "symlink", "old.txt");
    symlink_state.local_modified_ticks = 0;
    symlink_items.items.emplace("symlink", std::move(symlink_state));
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {file("symlink", "new.txt", 4)};
    FakeMetrics symlink_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(symlink_root, false),
            symlink_graph,
            symlink_items,
            symlink_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        !std::filesystem::is_symlink(symlink_root / "old.txt") ||
        std::filesystem::exists(symlink_root / "new.txt") ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move followed or replaced a symbolic link");
    }

    const auto shared_memory_root = std::filesystem::path{"/dev/shm"};
    std::error_code device_error;
    if (std::filesystem::is_directory(shared_memory_root, device_error) &&
        !device_error) {
        const auto cross_source_root = temporary.path() / "cross-device-source";
        std::filesystem::create_directories(cross_source_root);
        const auto cross_source = cross_source_root / "old.txt";
        {
            std::ofstream output{cross_source};
            output << "data";
        }
        const auto cross_destination_root =
            shared_memory_root /
            ("onedrive-cpp-" + temporary.path().filename().string());
        struct DestinationCleanup {
            std::filesystem::path path;
            ~DestinationCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{cross_destination_root};
        std::filesystem::create_directory(cross_destination_root);
        const auto cross_destination = cross_destination_root / "new.txt";
        auto cross_change = file(
            "cross-device",
            cross_destination.lexically_relative("/").generic_string(),
            4
        );
        FakeGraphClient cross_graph;
        cross_graph.changes = {cross_change};
        FakeItemStore cross_items;
        cross_items.saved_delta_link = "saved";
        auto cross_state =
            tracked_item(cross_source_root, "cross-device", "old.txt");
        cross_state.remote_path =
            cross_source.lexically_relative("/").generic_string();
        cross_items.items.emplace("cross-device", std::move(cross_state));
        FakeMetrics cross_metrics;
        auto cross_config = config_for("/", false);
        cross_config.nosync_enabled = false;
        cross_config.sync_permissions =
            onedrive::config::SyncPermissionsMode::umask;
        if (onedrive::sync::SyncEngine{
                cross_config,
                cross_graph,
                cross_items,
                cross_metrics,
                &default_console
            }
                    .synchronize() != 2 ||
            !std::filesystem::exists(cross_source) ||
            std::filesystem::exists(cross_destination) ||
            cross_items.applied_delta.blocked_upserts.size() != 1 ||
            cross_items.applied_delta.blocked_upserts[0].reason_code !=
                "cross_device_move" ||
            !cross_items.pending_moves_by_id.empty() ||
            !cross_metrics.last_success) {
            return fail("cross-device remote move was not blocked safely");
        }
    }
    return EXIT_SUCCESS;
}

int test_local_move_uploads() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
    new_directory_parent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
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
        onedrive::sync::detail::SyncList::load(
            sync_list,
            onedrive::sync::detail::root_file_policy(
                selective_config.sync_root_files
            )
        )
            .fingerprint();
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
        onedrive::sync::detail::SyncList::load(
            sync_list,
            onedrive::sync::detail::root_file_policy(
                selective_parent_config.sync_root_files
            )
        )
            .fingerprint();
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

} // namespace

int main() {
    if (const int result = test_remote_moves(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_move_uploads(); result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
