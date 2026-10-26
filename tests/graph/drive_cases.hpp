#pragma once

namespace onedrive::test::graph::drive_cases {

int test_unsupported_root_and_path_metadata();
int test_onenote_ancestor_path_lookup();
int test_list_root_with_refresh_and_pagination();
int test_access_token_is_reused_across_graph_operations();
int test_access_token_is_refreshed_before_expiry_margin();
int test_drive_information_and_quota();
int test_business_drive_information_and_identity();
int test_sharepoint_drive_information_and_identity();
int test_rejects_invalid_drive_metadata();
int test_item_lookup_by_encoded_path();
int test_directory_creation();
int test_item_deletion();
int test_item_move();
int test_drive_operations_propagate_cancellation();
int test_drive_identity_and_profile_photo();
int test_drive_identity_types();
int test_drive_identity_cancellation();
int test_profile_photo_rejects_invalid_responses();
int test_business_drive_permission_errors();
int test_sharepoint_library_permission_errors();

} // namespace onedrive::test::graph::drive_cases
