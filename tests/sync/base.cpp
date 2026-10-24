#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/ui/cli/backend.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "base_cases.hpp"

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;
using namespace onedrive::test::sync::base_cases;

int main() {
    if (const int result = test_engine_owns_configuration();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_sync_cancellation_does_not_commit_delta();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pre_cancelled_sync_does_not_start();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_sync_cancellation_at_execution_boundary();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_remote_content_tag_strategy();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_refreshes_delta_state();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_filter_policies_apply_in_both_directions();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_remote_moves();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_malware_file_is_blocked_without_overwriting_local_data();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_download_failure_preserves_completed_items();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_independent_download_reuse();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_download_time_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_download_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_download_conflict_backup();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_download_time_backup();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_download_parent_symlink();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_account_layout_symlink();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_blocked_items_continue_and_retry();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_delta_cursor_restarts_full_query();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
