#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

int test_missing_authentication() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{}
        );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::nullopt)),
        auth_options(),
    };

    try {
        static_cast<void>(client.list_root());
        return fail("missing authentication was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("onedrive-cpp auth")) {
            return fail("missing authentication error was not actionable");
        }
    }
    if (!transport_pointer->queued.requests.empty()) {
        return fail("missing authentication unexpectedly made an HTTP request");
    }
    return EXIT_SUCCESS;
}
int test_invalid_file_hash() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        token_response(),
        graph_page(
            R"json([{"id":"file-id","name":"bad.txt","eTag":"etag","file":{"hashes":{"quickXorHash":"not-base64"}}}])json"
        ),
    });
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
    };
    try {
        static_cast<void>(client.list_root());
        return fail("invalid Graph file hash was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("invalid quickXorHash")) {
            return fail("invalid Graph file hash error was not actionable");
        }
    }
    return EXIT_SUCCESS;
}
int test_graph_error() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_error_response(
                403, "accessDenied", "The caller is not permitted"
            ),
        });
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
    };

    try {
        static_cast<void>(client.list_root());
        return fail("Graph API error was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("not permitted")) {
            return fail("Graph API error message was not reported");
        }
    }
    return EXIT_SUCCESS;
}
int test_throttling_retry_after() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_error_response(
                429,
                "activityLimitReached",
                "Rate limit exceeded",
                {{"Retry-After", "3"}}
            ),
            graph_page("[]"),
        });
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {},
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };

    if (!client.list_root().empty() ||
        sleeps != std::vector{std::chrono::seconds{3}} ||
        transport_pointer->queued.requests.size() != 3) {
        return fail("Graph Retry-After throttling was not retried correctly");
    }
    return EXIT_SUCCESS;
}
int test_throttling_fallback_and_limit() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_error_response(429, "", "Rate limit exceeded"),
            graph_error_response(
                429,
                "",
                "Rate limit exceeded",
                {{"retry-after", "invalid"}}
            ),
            graph_error_response(429, "", "Rate limit exceeded"),
        });
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.microsoft.com/v1.0",
            .maximum_throttle_retries = 2,
            .initial_throttle_delay = std::chrono::seconds{1},
            .maximum_throttle_delay = std::chrono::seconds{10},
        },
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };

    try {
        static_cast<void>(client.list_root());
        return fail("persistent Graph throttling was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("after 2 retries")) {
            return fail("throttling exhaustion was not reported");
        }
    }
    if (sleeps != std::vector{
                      std::chrono::seconds{1},
                      std::chrono::seconds{2},
                  }) {
        return fail(
            "Graph throttling fallback did not use exponential backoff"
        );
    }
    return EXIT_SUCCESS;
}
int test_transient_service_retries() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{.status_code = 408},
            onedrive::http::HttpResponse{.status_code = 502},
            onedrive::http::HttpResponse{
                .status_code = 503,
                .headers = {{"Retry-After", "3"}},
            },
            onedrive::http::HttpResponse{.status_code = 504},
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"value":[]})",
            },
        });
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .maximum_throttle_retries = 4,
            .initial_throttle_delay = std::chrono::seconds{1},
            .maximum_throttle_delay = std::chrono::seconds{10},
        },
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };

    if (!client.list_root().empty() ||
        sleeps !=
            std::vector{
                std::chrono::seconds{1},
                std::chrono::seconds{2},
                std::chrono::seconds{3},
                std::chrono::seconds{8},
            } ||
        transport_pointer->queued.requests.size() != 6) {
        return fail(
            "transient Graph service errors were not retried correctly"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_missing_authentication();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_file_hash(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_graph_error(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_throttling_retry_after();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_throttling_fallback_and_limit();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_transient_service_retries();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
