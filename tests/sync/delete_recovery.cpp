#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace onedrive::test::sync::delete_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_local_delete_journal_failure() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
            journal_failure_metrics,
            &default_console
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
        journal_failure_metrics,
        &default_console
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
    return EXIT_SUCCESS;
}

int test_local_delete_cancellation() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto cancelled_delete_root = temporary.path() / "cancelled-delete";
    std::filesystem::create_directories(cancelled_delete_root);
    {
        std::ofstream output{cancelled_delete_root / "removed.txt"};
        output << "removed";
    }
    FakeItemStore cancelled_delete_items;
    cancelled_delete_items.saved_delta_link = "saved";
    cancelled_delete_items.items.emplace(
        "cancelled-delete",
        tracked_item(cancelled_delete_root, "cancelled-delete", "removed.txt")
    );
    FakeGraphClient cancelled_delete_graph;
    FakeMetrics cancelled_delete_metrics;
    auto cancelled_delete_config = config_for(cancelled_delete_root, false);
    cancelled_delete_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    onedrive::sync::SyncEngine cancelled_delete_engine{
        cancelled_delete_config,
        cancelled_delete_graph,
        cancelled_delete_items,
        cancelled_delete_metrics,
        &default_console
    };
    if (cancelled_delete_engine.synchronize() != exit_code::success) {
        return fail("could not prepare remote deletion cancellation test");
    }
    std::filesystem::remove(cancelled_delete_root / "removed.txt");
    std::stop_source delete_cancellation;
    cancelled_delete_graph.during_remote_delete = [&delete_cancellation] {
        delete_cancellation.request_stop();
    };
    if (cancelled_delete_engine.synchronize(delete_cancellation.get_token()) !=
            exit_code::cancelled ||
        cancelled_delete_items.pending_deletes_by_id.size() != 1 ||
        !cancelled_delete_graph.deleted_items.empty() ||
        cancelled_delete_metrics.last_outcome !=
            onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail(
            "cancelled remote deletion did not retain its recovery journal"
        );
    }
    std::stop_source delete_recovery_cancellation;
    cancelled_delete_graph.during_remote_delete =
        [&delete_recovery_cancellation] {
            delete_recovery_cancellation.request_stop();
        };
    if (cancelled_delete_engine.synchronize(
            delete_recovery_cancellation.get_token()
        ) != exit_code::cancelled ||
        cancelled_delete_items.pending_deletes_by_id.size() != 1 ||
        cancelled_delete_metrics.last_outcome !=
            onedrive::metrics::SyncRunOutcome::cancelled) {
        return fail(
            "cancelled pending deletion recovery did not retain its journal"
        );
    }
    return EXIT_SUCCESS;
}

int test_local_delete_commit_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics,
            &default_console
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
        recovery_config,
        recovery_graph,
        recovery_items,
        recovery_metrics,
        &default_console
    }
                          .synchronize());
    if (recovery_graph.deleted_items.size() != 2 ||
        recovery_items.find("me", "recover-delete") ||
        !recovery_items.pending_deletes_by_id.empty()) {
        return fail("pending deletion was not recovered idempotently");
    }
    return EXIT_SUCCESS;
}

int test_local_delete_reappeared() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
            reappeared_metrics,
            &default_console
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
    return EXIT_SUCCESS;
}

int test_local_delete_conflict() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics,
            &default_console
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

int test_sharepoint_local_delete_permission() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const std::string sp_drive_id = "b!sp-library";
    const auto permission_root =
        temporary.path() / "sharepoint-delete-permission";
    std::filesystem::create_directories(permission_root);
    {
        std::ofstream output{permission_root / "readonly.txt"};
        output << "read";
    }
    {
        std::ofstream output{permission_root / "writable.txt"};
        output << "edit";
    }
    FakeItemStore permission_items;
    permission_items.saved_delta_link = "saved";
    auto readonly_item =
        tracked_item(permission_root, "readonly-delete", "readonly.txt");
    readonly_item.drive_id = sp_drive_id;
    auto writable_item =
        tracked_item(permission_root, "writable-delete", "writable.txt");
    writable_item.drive_id = sp_drive_id;
    permission_items.items.emplace("readonly-delete", std::move(readonly_item));
    permission_items.items.emplace("writable-delete", std::move(writable_item));
    std::filesystem::remove(permission_root / "readonly.txt");
    std::filesystem::remove(permission_root / "writable.txt");
    FakeGraphClient permission_graph;
    permission_graph.delete_permission_denied_id = "readonly-delete";
    FakeMetrics permission_metrics;
    auto permission_config = config_for(permission_root, false);
    permission_config.drive_id = sp_drive_id;
    permission_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !permission_metrics.last_success ||
        !permission_items.pending_deletes_by_id.empty() ||
        !permission_items.find(sp_drive_id, "readonly-delete") ||
        permission_items.find(sp_drive_id, "writable-delete") ||
        permission_items.blocked.size() != 1 ||
        permission_items.blocked.front().drive_id != sp_drive_id ||
        permission_items.blocked.front().remote_path != "readonly.txt" ||
        permission_items.blocked.front().reason_code !=
            "upload_delete_permission") {
        return fail(
            "SharePoint remote deletion permission failure aborted sync or "
            "dropped tracked state"
        );
    }
    permission_graph.delete_permission_denied_id.clear();
    if (onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        permission_items.find(sp_drive_id, "readonly-delete") ||
        !permission_items.blocked.empty()) {
        return fail(
            "restored SharePoint remote delete permission did not clear "
            "blocked deletion"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::delete_cases
