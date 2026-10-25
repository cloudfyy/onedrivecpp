#pragma once

namespace onedrive::test::sync::upload_cases {

int test_upload_progress_pipeline();
int test_local_change_during_upload();
int test_pending_upload_recovery();
int test_missing_upload_source();
int test_upload_checkpoint_recovery();
int test_pending_upload_recovery_conflict();
int test_external_cancellation_preserves_upload_journal();
int test_upload_preview();
int test_new_and_modified_uploads();
int test_upload_concurrency();
int test_fatal_upload_stops_workers();
int test_upload_quota_recovery();
int test_upload_snapshot_recovery();
int test_upload_permission_recovery();
int test_removed_upload_resource();
int test_directory_upload_journal_failure();
int test_nested_directory_uploads();
int test_directory_upload_quota_recovery();
int test_directory_upload_conflict();
int test_directory_upload_commit_recovery();
int test_directory_upload_selective_boundary();
int test_directory_upload_type_conflict();
int test_directory_changed_during_upload();
int test_directory_upload_recovery_conflict();
int test_business_server_enriched_file_operations();
int test_sharepoint_server_enriched_file_operations();

} // namespace onedrive::test::sync::upload_cases
