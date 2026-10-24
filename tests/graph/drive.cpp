#include "support.hpp"

#include <ranges>
#include <stop_token>
#include "drive_cases.hpp"

using namespace onedrive::test::graph;
using namespace onedrive::test::graph::drive_cases;

int main() {
    if (test_unsupported_root_and_path_metadata() != EXIT_SUCCESS ||
        test_onenote_ancestor_path_lookup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_list_root_with_refresh_and_pagination();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_access_token_is_reused_across_graph_operations();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_access_token_is_refreshed_before_expiry_margin();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_drive_information_and_quota();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_rejects_invalid_drive_metadata();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_lookup_by_encoded_path();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_creation(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_deletion(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_move(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_drive_operations_propagate_cancellation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_drive_identity_and_profile_photo();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (test_drive_identity_types() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_drive_identity_cancellation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_profile_photo_rejects_invalid_responses();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
