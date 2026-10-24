#pragma once

namespace onedrive::test::graph::delta_cases {

int test_delta_mixed_metadata_capabilities();
int test_business_delta_paths_across_pages();
int test_business_delta_saved_and_fetched_ancestors();
int test_business_delta_moves_and_restart();
int test_delta_rejects_malformed_sizes();
int test_delta_cached_ancestor_validation();
int test_delta_injected_ancestor_failures();
int test_business_delta_invalid_ancestry_and_cancellation();
int test_unsupported_delta_across_pages();
int test_onenote_partial_delta_ancestry();
int test_onenote_ancestor_lookup_failures();
int test_shared_delta_changes();
int test_delta_transport_and_shape_failures();
int test_delta_cancelled_before_request_or_between_pages();
int test_untrusted_pagination_url();
int test_invalid_root_page_shapes();
int test_delta_with_pagination();
int test_delta_requires_valid_file_system_modified_time();
int test_delta_resume_and_url_validation();
int test_invalid_delta_cursor_error();
int test_delta_request_cancellation();

} // namespace onedrive::test::graph::delta_cases
