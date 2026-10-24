#include "support.hpp"
#include "move_cases.hpp"
#include "onedrive/exit_code.hpp"
#include <algorithm>

namespace onedrive::test::sync::move_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_move_journal_failure() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
    return EXIT_SUCCESS;
}

int test_remote_move_cancellation() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto cancelled_move_root = temporary.path() / "cancelled-move";
    std::filesystem::create_directories(cancelled_move_root);
    {
        std::ofstream output{cancelled_move_root / "before.txt"};
        output << "data";
    }
    FakeItemStore cancelled_move_items;
    cancelled_move_items.saved_delta_link = "saved";
    cancelled_move_items.items.emplace(
        "cancelled-move",
        tracked_item(cancelled_move_root, "cancelled-move", "before.txt")
    );
    FakeGraphClient cancelled_move_graph;
    FakeMetrics cancelled_move_metrics;
    auto cancelled_move_config = config_for(cancelled_move_root, false);
    cancelled_move_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    onedrive::sync::SyncEngine cancelled_move_engine{
        cancelled_move_config,
        cancelled_move_graph,
        cancelled_move_items,
        cancelled_move_metrics,
        &default_console
    };
    if (cancelled_move_engine.synchronize() != exit_code::success) {
        return fail("could not prepare remote move cancellation test");
    }
    std::filesystem::rename(
        cancelled_move_root / "before.txt", cancelled_move_root / "after.txt"
    );
    std::stop_source move_cancellation;
    cancelled_move_graph.during_remote_move = [&move_cancellation] {
        move_cancellation.request_stop();
    };
    if (cancelled_move_engine.synchronize(move_cancellation.get_token()) !=
            exit_code::cancelled ||
        cancelled_move_items.pending_remote_moves_by_id.size() != 1 ||
        !cancelled_move_graph.moved_remote_items.empty() ||
        cancelled_move_metrics.last_outcome !=
            onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail(
            "cancelled remote move did not retain its recovery journal"
        );
    }
    std::stop_source move_recovery_cancellation;
    cancelled_move_graph.during_remote_move = [&move_recovery_cancellation] {
        move_recovery_cancellation.request_stop();
    };
    if (cancelled_move_engine.synchronize(
            move_recovery_cancellation.get_token()
        ) != exit_code::cancelled ||
        cancelled_move_items.pending_remote_moves_by_id.size() != 1 ||
        cancelled_move_metrics.last_outcome !=
            onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail(
            "cancelled pending move recovery did not retain its journal"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_move_commit_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::success ||
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
                .synchronize() != exit_code::success ||
        std::filesystem::exists(recovery_root / "new.txt") ||
        !deleted_move_items.pending_moves_by_id.empty() ||
        deleted_move_items.applied_delta.removals !=
            std::vector<std::string>{"move-recovery"}) {
        return fail("remotely deleted pending move target was not removed");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_identity_mismatch() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::blocked ||
        mismatched_items.applied_delta.blocked_upserts.size() != 1 ||
        mismatched_items.applied_delta.blocked_upserts[0].reason_code !=
            "pending_move_conflict" ||
        mismatched_items.pending_moves_by_id.size() != 1) {
        return fail("mismatched pending move destination was adopted");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_journaled_staging() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::success ||
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
    return EXIT_SUCCESS;
}

int test_remote_move_staging_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::success ||
        std::filesystem::exists(staged_path) ||
        !staged_recovery_items.pending_moves_by_id.empty() ||
        !staged_recovery_metrics.last_success) {
        return fail("partially staged move cycle was not recovered");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_staging_deletion() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::success ||
        std::filesystem::exists(staged_deletion_path) ||
        !staged_deletion_items.pending_moves_by_id.empty() ||
        staged_deletion_items.applied_delta.removals !=
            std::vector<std::string>{"staged-deletion"}) {
        return fail("remotely deleted staged move was not removed");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_cycle_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

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
                .synchronize() != exit_code::success ||
        !recovered_cycle_items.pending_moves_by_id.empty() ||
        recovered_cycle_graph.download_count != 0 ||
        !recovered_cycle_metrics.last_success) {
        return fail("staged move cycle was not recovered after restart");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::move_cases
