#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public onedrive::http::HttpTransport {
public:
    explicit FakeTransport(std::deque<onedrive::http::HttpResult> responses)
        : responses_{std::move(responses)} {}

    onedrive::http::HttpResult perform(
        const onedrive::http::HttpRequest& request
    ) const override {
        requests.push_back(request);
        if (responses_.empty()) {
            return std::unexpected(
                onedrive::http::HttpError{.message = "no fake response available"}
            );
        }
        auto response = std::move(responses_.front());
        responses_.pop_front();
        return response;
    }

    mutable std::vector<onedrive::http::HttpRequest> requests;

private:
    mutable std::deque<onedrive::http::HttpResult> responses_;
};

class FakeTokenStore final : public onedrive::auth::TokenStore {
public:
    explicit FakeTokenStore(std::optional<std::string> refresh_token)
        : refresh_token_{std::move(refresh_token)} {}

    [[nodiscard]] std::optional<std::string> load_refresh_token() const override {
        return refresh_token_;
    }

    void save_refresh_token(const std::string& refresh_token) const override {
        saved_tokens.push_back(refresh_token);
        refresh_token_ = refresh_token;
    }

    [[nodiscard]] bool remove_refresh_token() const override {
        const bool present = refresh_token_.has_value();
        refresh_token_.reset();
        return present;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept override {
        return path_;
    }

    mutable std::vector<std::string> saved_tokens;

private:
    mutable std::optional<std::string> refresh_token_;
    std::filesystem::path path_{"/fake/refresh_token"};
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

onedrive::auth::DeviceAuthOptions auth_options() {
    return {
        .application_id = "client id",
        .tenant_id = "test-tenant",
        .auth_endpoint = "https://login.example.test",
        .scope = "Files.ReadWrite offline_access",
    };
}

bool has_header(
    const onedrive::http::HttpRequest& request,
    const std::string& expected
) {
    for (const auto& header : request.headers) {
        if (header == expected) {
            return true;
        }
    }
    return false;
}

int test_list_root_with_refresh_and_pagination() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"rotated-refresh"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"value":[{"id":"folder-id","name":"Documents",)"
                    R"("eTag":"folder-etag","folder":{"childCount":2}}],)"
                    R"("@odata.nextLink":)"
                    R"("https://graph.example.test/v1.0/next?page=2"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"value":[{"id":"file-id","name":"notes.txt",)"
                    R"("eTag":"file-etag","file":{"mimeType":"text/plain"}}]})",
            },
        }
    );
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();

    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::move(token_store),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0/",
        },
    };
    const auto items = client.list_root();

    if (items.size() != 2 || !items[0].directory || items[1].directory ||
        items[0].name != "Documents" || items[1].etag != "file-etag") {
        return fail("Graph drive items were not parsed across pages");
    }
    if (token_store_pointer->saved_tokens !=
        std::vector<std::string>{"rotated-refresh"}) {
        return fail("rotated refresh token was not persisted");
    }
    if (transport_pointer->requests.size() != 3 ||
        transport_pointer->requests[0].method !=
            onedrive::http::HttpMethod::post ||
        !transport_pointer->requests[0].body.contains(
            "refresh_token=existing-refresh"
        ) ||
        transport_pointer->requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/children" ||
        transport_pointer->requests[2].url !=
            "https://graph.example.test/v1.0/next?page=2" ||
        !has_header(
            transport_pointer->requests[1],
            "Authorization: Bearer access-secret"
        )) {
        return fail("Graph authentication or pagination request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_missing_authentication() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{}
    );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::make_unique<FakeTokenStore>(std::nullopt),
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
    if (!transport_pointer->requests.empty()) {
        return fail("missing authentication unexpectedly made an HTTP request");
    }
    return EXIT_SUCCESS;
}

int test_graph_error() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 403,
                .body =
                    R"({"error":{"code":"accessDenied",)"
                    R"("message":"The caller is not permitted"}})",
            },
        }
    );
    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}),
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

int test_untrusted_pagination_url() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"value":[],"@odata.nextLink":)"
                    R"("https://attacker.example/collect"})",
            },
        }
    );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}),
        auth_options(),
    };

    try {
        static_cast<void>(client.list_root());
        return fail("untrusted Graph pagination URL was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("outside its endpoint")) {
            return fail("untrusted pagination URL error was not reported");
        }
    }
    if (transport_pointer->requests.size() != 2) {
        return fail("authorization token was sent to an untrusted pagination URL");
    }
    return EXIT_SUCCESS;
}

int test_throttling_retry_after() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 429,
                .headers = {{"Retry-After", "3"}},
                .body =
                    R"({"error":{"code":"activityLimitReached",)"
                    R"("message":"Rate limit exceeded"}})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"value":[]})",
            },
        }
    );
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}),
        auth_options(),
        {},
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };

    if (!client.list_root().empty() ||
        sleeps != std::vector{std::chrono::seconds{3}} ||
        transport_pointer->requests.size() != 3) {
        return fail("Graph Retry-After throttling was not retried correctly");
    }
    return EXIT_SUCCESS;
}

int test_throttling_fallback_and_limit() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 429,
                .body = R"({"error":{"message":"Rate limit exceeded"}})",
            },
            onedrive::http::HttpResponse{
                .status_code = 429,
                .headers = {{"retry-after", "invalid"}},
                .body = R"({"error":{"message":"Rate limit exceeded"}})",
            },
            onedrive::http::HttpResponse{
                .status_code = 429,
                .body = R"({"error":{"message":"Rate limit exceeded"}})",
            },
        }
    );
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        std::move(transport),
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}),
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
    if (sleeps !=
        std::vector{
            std::chrono::seconds{1},
            std::chrono::seconds{2},
        }) {
        return fail("Graph throttling fallback did not use exponential backoff");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_list_root_with_refresh_and_pagination();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_missing_authentication(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_graph_error(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_untrusted_pagination_url();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_throttling_retry_after();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_throttling_fallback_and_limit();
}
