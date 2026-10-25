#pragma once

namespace onedrive::test::sync::move_cases {

int test_remote_move_renames();
int test_remote_move_journal_failure();
int test_remote_move_cancellation();
int test_remote_move_commit_recovery();
int test_remote_move_adoption();
int test_remote_move_identity_mismatch();
int test_remote_move_changed_content();
int test_remote_move_directory();
int test_remote_move_chain();
int test_remote_move_chain_recovery();
int test_remote_move_nested();
int test_remote_move_cycle();
int test_remote_move_three_cycle();
int test_remote_move_directory_cycle();
int test_remote_move_journaled_staging();
int test_remote_move_staging_recovery();
int test_remote_move_staging_deletion();
int test_remote_move_cycle_recovery();
int test_remote_move_blocked_dependency();
int test_remote_move_modified_source();
int test_remote_move_collision();
int test_remote_move_preview();
int test_remote_move_symlink();
int test_remote_move_cross_device();
int test_local_move_rename_and_preview();
int test_local_move_new_parent();
int test_local_move_tracked_parent();
int test_local_move_directory_new_parent();
int test_local_move_recovery();
int test_local_move_directory();
int test_local_move_selective_boundaries();
int test_local_move_parent_recovery();

int test_business_remote_move_enriched_file();
int test_sharepoint_remote_move_enriched_file();
int test_sharepoint_local_move_permission();

} // namespace onedrive::test::sync::move_cases
