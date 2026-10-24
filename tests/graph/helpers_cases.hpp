#pragma once

namespace onedrive::test::graph::helper_cases {

int test_unsupported_formats();
int test_ancestor_metadata_validation();
int test_ancestor_failure_and_cancellation_boundaries();
int test_unsupported_ancestry();
int test_repeated_page_fetcher();
int test_paths_and_errors();
int test_status_and_retry_helpers();
int test_json_and_facets();
int test_hashes_and_drive_items();

} // namespace onedrive::test::graph::helper_cases
