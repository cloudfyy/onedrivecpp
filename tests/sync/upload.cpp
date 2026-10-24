#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;
using namespace onedrive::test::sync::upload_cases;

int main() {
    onedrive::test::ScenarioProgress progress{24};
    if (progress.run(
            "upload_progress_pipeline", test_upload_progress_pipeline
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result =
            progress.run("missing_upload_source", test_missing_upload_source);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run("upload_preview", test_upload_preview);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "new_and_modified_uploads", test_new_and_modified_uploads
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("upload_concurrency", test_upload_concurrency);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "fatal_upload_stops_workers", test_fatal_upload_stops_workers
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("upload_quota_recovery", test_upload_quota_recovery);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "upload_snapshot_recovery", test_upload_snapshot_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "upload_permission_recovery", test_upload_permission_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "removed_upload_resource", test_removed_upload_resource
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "external_cancellation_preserves_upload_journal",
            test_external_cancellation_preserves_upload_journal
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_journal_failure",
            test_directory_upload_journal_failure
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "nested_directory_uploads", test_nested_directory_uploads
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_quota_recovery",
            test_directory_upload_quota_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_conflict", test_directory_upload_conflict
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_commit_recovery",
            test_directory_upload_commit_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_selective_boundary",
            test_directory_upload_selective_boundary
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_type_conflict",
            test_directory_upload_type_conflict
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_changed_during_upload",
            test_directory_changed_during_upload
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "directory_upload_recovery_conflict",
            test_directory_upload_recovery_conflict
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_change_during_upload", test_local_change_during_upload
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "pending_upload_recovery", test_pending_upload_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "upload_checkpoint_recovery", test_upload_checkpoint_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "pending_upload_recovery_conflict",
            test_pending_upload_recovery_conflict
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
