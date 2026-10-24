#include "support/progress.hpp"
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
    onedrive::test::ScenarioProgress progress{10};
    const int authentication =
        progress.run("account_authentication_error_type", [] {
            if (!throws_with<onedrive::graph::AccountAuthenticationError>(
                    [] {
                        detail::require_successful_graph_response(
                            Json{
                                {"error",
                                 {{"message", "account token rejected"}}}
                            },
                            401,
                            "delta"
                        );
                    },
                    "account token rejected"
                )) {
                return fail(
                    "Graph authentication failure lost its account-wide type"
                );
            }
            return EXIT_SUCCESS;
        });
    if (authentication != EXIT_SUCCESS) {
        return authentication;
    }
    if (progress.run("unsupported_formats", test_unsupported_formats) !=
            EXIT_SUCCESS ||
        progress.run("unsupported_ancestry", test_unsupported_ancestry) !=
            EXIT_SUCCESS ||
        progress.run(
            "ancestor_metadata_validation", test_ancestor_metadata_validation
        ) != EXIT_SUCCESS ||
        progress.run(
            "ancestor_failure_and_cancellation_boundaries",
            test_ancestor_failure_and_cancellation_boundaries
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result =
            progress.run("repeated_page_fetcher", test_repeated_page_fetcher);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("paths_and_errors", test_paths_and_errors);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "status_and_retry_helpers", test_status_and_retry_helpers
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("json_and_facets", test_json_and_facets);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(
        progress.run("hashes_and_drive_items", test_hashes_and_drive_items)
    );
}
