#include "support/progress.hpp"
#include "move_cases.hpp"
#include <cstdlib>

using namespace onedrive::test::sync::move_cases;

int main() {
    onedrive::test::ScenarioProgress progress{36};
    if (const int result =
            progress.run("remote_move_renames", test_remote_move_renames);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_journal_failure", test_remote_move_journal_failure
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_cancellation", test_remote_move_cancellation
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_commit_recovery", test_remote_move_commit_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_adoption", test_remote_move_adoption);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_identity_mismatch", test_remote_move_identity_mismatch
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_changed_content", test_remote_move_changed_content
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_directory", test_remote_move_directory);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_chain", test_remote_move_chain);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_chain_recovery", test_remote_move_chain_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_nested", test_remote_move_nested);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_cycle", test_remote_move_cycle);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_three_cycle", test_remote_move_three_cycle
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_directory_cycle", test_remote_move_directory_cycle
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_journaled_staging", test_remote_move_journaled_staging
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_staging_recovery", test_remote_move_staging_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_staging_deletion", test_remote_move_staging_deletion
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_cycle_recovery", test_remote_move_cycle_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_blocked_dependency",
            test_remote_move_blocked_dependency
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_modified_source", test_remote_move_modified_source
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_collision", test_remote_move_collision);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_preview", test_remote_move_preview);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("remote_move_symlink", test_remote_move_symlink);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "remote_move_cross_device", test_remote_move_cross_device
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_move_rename_and_preview", test_local_move_rename_and_preview
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("local_move_new_parent", test_local_move_new_parent);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_move_tracked_parent", test_local_move_tracked_parent
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_move_directory_new_parent",
            test_local_move_directory_new_parent
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("local_move_recovery", test_local_move_recovery);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("local_move_directory", test_local_move_directory);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_move_selective_boundaries",
            test_local_move_selective_boundaries
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "local_move_parent_recovery", test_local_move_parent_recovery
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "business_remote_move_enriched_file",
            test_business_remote_move_enriched_file
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "sharepoint_remote_move_enriched_file",
            test_sharepoint_remote_move_enriched_file
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "business_local_move_permission",
            test_business_local_move_permission
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "sharepoint_local_move_permission",
            test_sharepoint_local_move_permission
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
