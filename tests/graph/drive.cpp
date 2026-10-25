#include "support/progress.hpp"
#include "support.hpp"

#include <ranges>
#include <stop_token>
#include "drive_cases.hpp"

using namespace onedrive::test::graph;
using namespace onedrive::test::graph::drive_cases;

int main() {
    onedrive::test::ScenarioProgress progress{19};
    if (progress.run(
            "unsupported_root_and_path_metadata",
            test_unsupported_root_and_path_metadata
        ) != EXIT_SUCCESS ||
        progress.run(
            "onenote_ancestor_path_lookup", test_onenote_ancestor_path_lookup
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = progress.run(
            "list_root_with_refresh_and_pagination",
            test_list_root_with_refresh_and_pagination
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "access_token_is_reused_across_graph_operations",
            test_access_token_is_reused_across_graph_operations
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "access_token_is_refreshed_before_expiry_margin",
            test_access_token_is_refreshed_before_expiry_margin
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "drive_information_and_quota", test_drive_information_and_quota
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "business_drive_information_and_identity",
            test_business_drive_information_and_identity
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "sharepoint_drive_information_and_identity",
            test_sharepoint_drive_information_and_identity
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "rejects_invalid_drive_metadata",
            test_rejects_invalid_drive_metadata
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "item_lookup_by_encoded_path", test_item_lookup_by_encoded_path
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("directory_creation", test_directory_creation);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run("item_deletion", test_item_deletion);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run("item_move", test_item_move);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "drive_operations_propagate_cancellation",
            test_drive_operations_propagate_cancellation
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "drive_identity_and_profile_photo",
            test_drive_identity_and_profile_photo
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (progress.run("drive_identity_types", test_drive_identity_types) !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = progress.run(
            "drive_identity_cancellation", test_drive_identity_cancellation
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "profile_photo_rejects_invalid_responses",
            test_profile_photo_rejects_invalid_responses
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "sharepoint_library_permission_errors",
            test_sharepoint_library_permission_errors
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
