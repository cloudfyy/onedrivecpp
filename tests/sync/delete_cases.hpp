#pragma once

namespace onedrive::test::sync::delete_cases {

int test_recursive_deletion_protection();
int test_full_refresh_retains_blocked_deletion_snapshot();
int test_remote_delete_unchanged();
int test_remote_delete_preview();
int test_remote_delete_full_refresh();
int test_remote_delete_partial_cleanup();
int test_remote_delete_orphan_partial();
int test_remote_delete_live_partial();
int test_remote_delete_unsafe_partial();
int test_remote_delete_partial_commit_failure();
int test_remote_delete_modified_retry();
int test_remote_delete_preserved_metadata();
int test_remote_delete_unverified_snapshot();
int test_remote_delete_inspection_failure();
int test_remote_delete_transferred_snapshot();
int test_remote_delete_tree();
int test_remote_delete_nonempty_directory();
int test_remote_delete_symlink();
int test_remote_delete_already_absent();
int test_remote_delete_untracked();
int test_local_delete_propagation();
int test_local_delete_preview();
int test_local_delete_large_guard();
int test_local_delete_selective_boundary();
int test_local_delete_filter_policy();
int test_local_delete_journal_failure();
int test_local_delete_cancellation();
int test_local_delete_commit_recovery();
int test_local_delete_reappeared();
int test_local_delete_conflict();

int test_business_remote_delete_enriched_unchanged();
int test_sharepoint_remote_delete_enriched_unchanged();
int test_business_local_delete_permission();
int test_sharepoint_local_delete_permission();

} // namespace onedrive::test::sync::delete_cases
