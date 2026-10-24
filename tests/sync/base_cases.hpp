#pragma once

namespace onedrive::test::sync::base_cases {

int test_engine_owns_configuration();
int test_sync_cancellation_does_not_commit_delta();
int test_pre_cancelled_sync_does_not_start();
int test_sync_cancellation_at_execution_boundary();
int test_dry_run_and_success();
int test_remote_content_tag_strategy();
int test_selective_sync_refreshes_delta_state();
int test_filter_policies_apply_in_both_directions();
int test_selective_sync_remote_moves();
int test_malware_file_is_blocked_without_overwriting_local_data();
int test_blocked_items_continue_and_retry();
int test_invalid_delta_cursor_restarts_full_query();
int test_download_failure_preserves_completed_items();
int test_independent_download_reuse();
int test_download_time_conflict();
int test_local_download_conflict();
int test_download_conflict_backup();
int test_download_time_backup();
int test_download_parent_symlink();
int test_account_layout_symlink();

} // namespace onedrive::test::sync::base_cases
