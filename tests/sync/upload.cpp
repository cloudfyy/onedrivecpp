#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;
using namespace onedrive::test::sync::upload_cases;

int main() {
    if (test_upload_progress_pipeline() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_missing_upload_source();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_preview(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_new_and_modified_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_concurrency(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_fatal_upload_stops_workers();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_quota_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_snapshot_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_permission_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_removed_upload_resource();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_external_cancellation_preserves_upload_journal();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_journal_failure();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_nested_directory_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_quota_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_commit_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_selective_boundary();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_type_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_changed_during_upload();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_upload_recovery_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_change_during_upload();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_checkpoint_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
