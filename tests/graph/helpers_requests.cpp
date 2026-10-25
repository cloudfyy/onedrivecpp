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

namespace onedrive::test::graph::helper_cases {

namespace detail = onedrive::graph::client_detail;
using detail::Json;
using onedrive::test::fail;

using onedrive::test::throws_with;

struct MoveOnlyPageFetcher {
    std::unique_ptr<std::size_t> calls = std::make_unique<std::size_t>(0);

    Json operator()(const std::string& url) & {
        ++*calls;
        if (*calls == 1 && url == "https://graph.example.test/page1") {
            return {
                {"value", Json::array({1})},
                {"@odata.nextLink", "https://graph.example.test/page2"},
            };
        }
        if (*calls == 2 && url == "https://graph.example.test/page2") {
            return {{"value", Json::array({2})}};
        }
        throw std::runtime_error{"pagination callable was reused incorrectly"};
    }
    Json operator()(const std::string&) && = delete;
};

int test_repeated_page_fetcher() {
    const auto fetch = [](auto operation) {
        return detail::paged_graph_values(
            "https://graph.example.test/page1",
            "https://graph.example.test",
            "test pages",
            std::move(operation)
        );
    };
    if (fetch(MoveOnlyPageFetcher{}) != std::vector<Json>{1, 2}) {
        return fail("owned move-only pagination callable lost its state");
    }
    MoveOnlyPageFetcher borrowed;
    if (fetch(std::ref(borrowed)) != std::vector<Json>{1, 2} ||
        *borrowed.calls != 2) {
        return fail("borrowed pagination callable lost its state");
    }
    std::size_t calls = 0;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(fetch([&](const std::string&) -> Json {
                    if (++calls == 2) {
                        throw std::runtime_error{"second page failed"};
                    }
                    return {
                        {"value", Json::array({1})},
                        {"@odata.nextLink", "https://graph.example.test/page2"},
                    };
                }));
            },
            "second page failed"
        ) ||
        calls != 2) {
        return fail("pagination swallowed or retried a fetch failure");
    }
    return EXIT_SUCCESS;
}

int test_paths_and_errors() {
    if (detail::percent_encode_remote_path("My Folder/report #1.txt") !=
            "My%20Folder/report%20%231.txt" ||
        detail::normalized_endpoint("https://graph.example.test///") !=
            "https://graph.example.test" ||
        detail::graph_drive_prefix({
            .drive_id = "me",
            .endpoint = "https://graph.example.test",
        }) != "https://graph.example.test/me/drive" ||
        detail::graph_drive_prefix({
            .drive_id = "shared drive",
            .endpoint = "https://graph.example.test",
        }) != "https://graph.example.test/drives/shared%20drive") {
        return fail("Graph path or endpoint helpers returned the wrong value");
    }
    for (const std::string_view path : {
             "",
             "/absolute",
             "trailing/",
             "empty//segment",
             "./file",
             "../file",
             "directory\\file",
         }) {
        if (!throws_with(
                [&] {
                    static_cast<void>(detail::percent_encode_remote_path(path));
                },
                "remote file path"
            )) {
            return fail("unsafe Graph path was accepted");
        }
    }

    const Json error = {
        {"error", {{"code", "quotaLimitReached"}, {"message", "full"}}},
    };
    for (const auto* code : {
             "QUOTALIMITREACHED",
             "sToRaGeLiMiTeXcEeDeD",
             "INSUFFICIENTSTORAGE",
         }) {
        const Json response{{"error", {{"code", code}}}};
        if (!detail::upload_quota_error(response, 403) ||
            detail::graph_error_code(response) != code) {
            return fail("Graph quota comparison changed or rejected its code");
        }
    }
    for (const auto* code : {
             "quotaLimitReachedExtra",
             " quotaLimitReached",
             "insufficientStorage ",
             "\xc4",
             "",
         }) {
        if (detail::upload_quota_error(
                Json{{"error", {{"code", code}}}}, 403
            )) {
            return fail("non-quota Graph code was classified as quota");
        }
    }
    if (detail::graph_error_message(error, 400) != "full" ||
        detail::graph_error_code(error) != "quotaLimitReached" ||
        detail::graph_error_message(Json::object(), 418) !=
            "Microsoft Graph request failed with HTTP 418" ||
        !detail::graph_error_code(Json{{"error", "invalid"}}).empty() ||
        !detail::upload_quota_error(Json::object(), 507) ||
        !detail::upload_quota_error(error, 400) ||
        !detail::upload_quota_error(
            Json{{"error", {{"code", "StorageLimitExceeded"}}}}, 400
        ) ||
        !detail::upload_quota_error(
            Json{{"error", {{"code", "insufficientstorage"}}}}, 400
        ) ||
        detail::upload_quota_error(Json::object(), 400)) {
        return fail("Graph error helpers classified a response incorrectly");
    }
    try {
        detail::throw_upload_response_error(error, 400);
        return fail("Graph quota response was accepted");
    } catch (const onedrive::graph::UploadResourceError& resource_error) {
        if (resource_error.reason_code() != "remote_quota") {
            return fail("Graph quota response used the wrong error code");
        }
    }
    const Json permission_error = {
        {"error", {{"code", "accessDenied"}, {"message", "read-only library"}}},
    };
    if (!detail::upload_permission_error(Json::object(), 403) ||
        !detail::upload_permission_error(permission_error, 400) ||
        !detail::upload_permission_error(
            Json{{"error", {{"code", "notAllowed"}}}}, 400
        ) ||
        detail::upload_permission_error(error, 403) ||
        detail::upload_permission_error(Json::object(), 400)) {
        return fail(
            "Graph permission helpers classified a response incorrectly"
        );
    }
    try {
        detail::throw_upload_response_error(permission_error, 403);
        return fail("Graph permission response was accepted");
    } catch (const onedrive::graph::UploadResourceError& resource_error) {
        if (resource_error.reason_code() != "remote_permission") {
            return fail("Graph permission response used the wrong error code");
        }
    }
    if (!throws_with(
            [&] { detail::throw_upload_response_error(Json::object(), 500); },
            "HTTP 500"
        )) {
        return fail("Graph upload server error was accepted");
    }
    return EXIT_SUCCESS;
}

int test_status_and_retry_helpers() {
    if (detail::successful_status(199) || !detail::successful_status(200) ||
        !detail::successful_status(299) || detail::successful_status(300) ||
        !detail::retryable_status(408) || !detail::retryable_status(429) ||
        !detail::retryable_status(502) || !detail::retryable_status(503) ||
        !detail::retryable_status(504) || detail::retryable_status(500) ||
        !detail::stale_download_url_status(401) ||
        !detail::stale_download_url_status(403) ||
        detail::stale_download_url_status(404)) {
        return fail("Graph status classification was incorrect");
    }
    detail::require_successful_graph_response(Json::object(), 204, "ignored");
    if (!throws_with(
            [&] {
                detail::require_successful_graph_response(Json::object(), 500);
            },
            "HTTP 500"
        ) ||
        !throws_with(
            [&] {
                detail::require_successful_graph_response(
                    Json::object(), 500, "listing files"
                );
            },
            "listing files: Microsoft Graph"
        )) {
        return fail("Graph failure response was accepted");
    }

    using onedrive::http::UploadTransportOptions;
    if (detail::effective_upload_rate({}) != 0 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_send_speed_bytes_per_second = 10,
            }
        ) != 10 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_total_send_speed_bytes_per_second = 20,
            }
        ) != 20 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_send_speed_bytes_per_second = 30,
                .maximum_total_send_speed_bytes_per_second = 20,
            }
        ) != 20) {
        return fail("effective Graph upload rate was incorrect");
    }

    const onedrive::http::HttpResponse response{
        .headers = {
            {.name = "Unrelated", .value = "ignored"},
            {.name = "retry-after", .value = "3"},
            {.name = "Request-Id", .value = "request"},
        },
    };
    if (detail::retry_after(response) != std::chrono::seconds{3} ||
        detail::header_value(response, "request-id") != "request" ||
        detail::header_value(response, "missing") ||
        detail::retry_after({
            .headers = {{.name = "Retry-After", .value = "-1"}},
        }) ||
        detail::retry_after({
            .headers = {{.name = "Retry-After", .value = "3 seconds"}},
        })) {
        return fail("Graph response headers were parsed incorrectly");
    }

    const onedrive::graph::GraphOptions options{
        .initial_throttle_delay = std::chrono::seconds{3},
        .maximum_throttle_delay = std::chrono::seconds{10},
    };
    if (detail::fallback_retry_delay(options, 0) != std::chrono::seconds{3} ||
        detail::fallback_retry_delay(options, 1) != std::chrono::seconds{6} ||
        detail::fallback_retry_delay(options, 2) != std::chrono::seconds{10} ||
        detail::fallback_retry_delay(options, 20) != std::chrono::seconds{10}) {
        return fail("Graph fallback retry delay was incorrect");
    }

    int sleeps = 0;
    if (detail::wait_for_retry(
            std::chrono::seconds::zero(),
            [&](std::chrono::seconds) { ++sleeps; },
            {}
        ) ||
        sleeps != 1) {
        return fail("uncancellable Graph retry did not use the sleep callback");
    }
    std::stop_source stop;
    stop.request_stop();
    if (!detail::wait_for_retry(
            std::chrono::seconds{1},
            [](std::chrono::seconds) {},
            stop.get_token()
        )) {
        return fail("cancelled Graph retry wait was not interrupted");
    }
    const auto cancelled = detail::cancelled_http_result();
    if (cancelled ||
        cancelled.error().code != onedrive::http::HttpErrorCode::cancelled) {
        return fail("cancelled Graph HTTP result was incorrect");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::graph::helper_cases
