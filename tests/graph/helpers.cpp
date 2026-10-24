#include "graph/support.hpp"
#include "graph/unsupported_formats.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "helpers_cases.hpp"

namespace detail = onedrive::graph::client_detail;
using detail::Json;
using onedrive::test::fail;

using onedrive::test::throws_with;
using namespace onedrive::test::graph::helper_cases;

int main() {
    if (!throws_with<onedrive::graph::AccountAuthenticationError>(
            [] {
                detail::require_successful_graph_response(
                    Json{{"error", {{"message", "account token rejected"}}}},
                    401,
                    "delta"
                );
            },
            "account token rejected"
        )) {
        return fail("Graph authentication failure lost its account-wide type");
    }
    if (test_unsupported_formats() != EXIT_SUCCESS ||
        test_unsupported_ancestry() != EXIT_SUCCESS ||
        test_ancestor_metadata_validation() != EXIT_SUCCESS ||
        test_ancestor_failure_and_cancellation_boundaries() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_repeated_page_fetcher();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_paths_and_errors(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_status_and_retry_helpers();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_json_and_facets(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_hashes_and_drive_items();
}
