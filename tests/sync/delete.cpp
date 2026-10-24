#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/orchestration.hpp"
#include "delete_cases.hpp"

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;
using namespace onedrive::test::sync::delete_cases;

int main() {
    onedrive::test::ScenarioProgress progress{26};
    if (const int result = progress.run(
            "full_refresh_retains_blocked_deletion_snapshot",
            test_full_refresh_retains_blocked_deletion_snapshot
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "recursive_deletion_protection", test_recursive_deletion_protection
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_unchanged", test_remote_delete_unchanged
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_delete_preview", test_remote_delete_preview);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_full_refresh", test_remote_delete_full_refresh
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_partial_cleanup", test_remote_delete_partial_cleanup
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_orphan_partial", test_remote_delete_orphan_partial
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_live_partial", test_remote_delete_live_partial
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_unsafe_partial", test_remote_delete_unsafe_partial
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_partial_commit_failure",
            test_remote_delete_partial_commit_failure
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_modified_retry", test_remote_delete_modified_retry
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_delete_tree", test_remote_delete_tree);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_nonempty_directory",
            test_remote_delete_nonempty_directory
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_delete_symlink", test_remote_delete_symlink);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_already_absent", test_remote_delete_already_absent
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_delete_untracked", test_remote_delete_untracked
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_propagation", test_local_delete_propagation
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("local_delete_preview", test_local_delete_preview);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_large_guard", test_local_delete_large_guard
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_selective_boundary",
            test_local_delete_selective_boundary
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_filter_policy", test_local_delete_filter_policy
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_journal_failure", test_local_delete_journal_failure
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_cancellation", test_local_delete_cancellation
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_commit_recovery", test_local_delete_commit_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_delete_reappeared", test_local_delete_reappeared
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("local_delete_conflict", test_local_delete_conflict);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
