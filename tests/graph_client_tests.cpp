#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

class FakeTransport final {
public:
    explicit FakeTransport(std::deque<onedrive::http::HttpResult> responses)
        : responses_{std::move(responses)} {}

    onedrive::http::HttpResult perform(
        const onedrive::http::HttpRequest& request
    ) const {
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

    onedrive::http::HttpResult download(
        const onedrive::http::HttpRequest& request,
        const std::filesystem::path& destination,
        const onedrive::http::DownloadProgress& progress
    ) const {
        download_requests.push_back(request);
        std::string body = download_body;
        long status_code = 200;
        for (const auto& header : request.headers) {
            if (!header.starts_with("Range: bytes=")) {
                continue;
            }
            const auto separator = header.find('-', 13);
            const auto start = std::stoull(header.substr(13, separator - 13));
            const auto end = std::stoull(header.substr(separator + 1));
            body = download_body.substr(
                static_cast<std::size_t>(start),
                static_cast<std::size_t>(end - start + 1)
            );
            status_code = 206;
        }
        if (request.download_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << body;
        } else {
            std::fstream output{
                destination,
                std::ios::binary | std::ios::in | std::ios::out
            };
            output.seekp(static_cast<std::streamoff>(request.download_offset));
            output << body;
        }
        if (progress) {
            progress(body.size(), body.size());
        }
        if (!download_responses.empty()) {
            auto response = std::move(download_responses.front());
            download_responses.pop_front();
            return response;
        }
        return onedrive::http::HttpResponse{.status_code = status_code};
    }

    mutable std::vector<onedrive::http::HttpRequest> requests;
    mutable std::vector<onedrive::http::HttpRequest> download_requests;
    mutable std::deque<onedrive::http::HttpResult> download_responses;
    std::string download_body{"download"};

private:
    mutable std::deque<onedrive::http::HttpResult> responses_;
};

class FakeTokenStore final {
public:
    explicit FakeTokenStore(std::optional<std::string> refresh_token)
        : refresh_token_{std::move(refresh_token)} {}

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        return refresh_token_;
    }

    void save_refresh_token(const std::string& refresh_token) const {
        saved_tokens.push_back(refresh_token);
        refresh_token_ = refresh_token;
    }

    [[nodiscard]] bool remove_refresh_token() const {
        const bool present = refresh_token_.has_value();
        refresh_token_.reset();
        return present;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    mutable std::vector<std::string> saved_tokens;

private:
    mutable std::optional<std::string> refresh_token_;
    std::filesystem::path path_{"/fake/refresh_token"};
};

template <typename Implementation>
std::unique_ptr<onedrive::http::HttpTransport> wrap_transport(
    std::unique_ptr<Implementation> implementation
) {
    return std::make_unique<onedrive::http::HttpTransport>(
        std::move(implementation)
    );
}

template <typename Implementation>
std::unique_ptr<onedrive::auth::TokenStore> wrap_token_store(
    std::unique_ptr<Implementation> implementation
) {
    return std::make_unique<onedrive::auth::TokenStore>(
        std::move(implementation)
    );
}

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
        if (header == expected ||
            (expected == "Authorization: ******" &&
             header.starts_with("Authorization: Bearer "))) {
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
                    R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA="}}}]})json",
            },
        }
    );
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::move(token_store)),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0/",
        },
    };
    const auto items = client.list_root();

    if (items.size() != 2 || !items[0].directory || items[1].directory ||
        items[0].name != "Documents" || items[1].etag != "file-etag" ||
        !items[1].content_hash ||
        items[1].content_hash->algorithm !=
            onedrive::FileHashAlgorithm::quick_xor ||
        items[1].content_hash->value !=
            "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
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
    if (!transport_pointer->requests.empty()) {
        return fail("missing authentication unexpectedly made an HTTP request");
    }
    return EXIT_SUCCESS;
}

int test_invalid_file_hash() {
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
                    R"json({"value":[{"id":"file-id","name":"bad.txt","eTag":"etag","file":{"hashes":{"quickXorHash":"not-base64"}}}]})json",
            },
        }
    );
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(
                std::string{"existing-refresh"}
            )
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
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
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
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
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
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
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
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
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

int test_transient_service_retries() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
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
        }
    );
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .maximum_throttle_retries = 3,
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
                std::chrono::seconds{3},
                std::chrono::seconds{4},
            } ||
        transport_pointer->requests.size() != 5) {
        return fail("transient Graph service errors were not retried correctly");
    }
    return EXIT_SUCCESS;
}

int test_delta_with_pagination() {
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
                    R"({"value":[{"id":"root-id","name":"Drive",)"
                    R"("size":0,"root":{},"folder":{"childCount":1}},)"
                    R"({"id":"folder-id","name":"Documents",)"
                    R"("size":0,)"
                    R"("lastModifiedDateTime":"2026-10-02T00:00:00Z",)"
                    R"("parentReference":{"id":"root-id",)"
                    R"("path":"/drives/drive-id/root:"},)"
                    R"("folder":{"childCount":1}}],)"
                    R"("@odata.nextLink":)"
                    R"("https://graph.example.test/v1.0/delta?page=2"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","size":42,"lastModifiedDateTime":"2026-10-02T00:01:00Z","parentReference":{"id":"folder-id","path":"/drives/drive-id/root:/Documents"},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA=","sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}},{"id":"deleted-id","deleted":{"state":"deleted"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?token=final"})json",
            },
        }
    );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    std::vector<std::tuple<std::size_t, std::size_t, bool>> progress;
    const auto delta = client.list_delta(
        std::nullopt,
        [&progress](std::size_t pages, std::size_t items, bool completed) {
            progress.emplace_back(pages, items, completed);
        }
    );
    if (delta.changes.size() != 4 ||
        delta.delta_link !=
            "https://graph.example.test/v1.0/delta?token=final" ||
        !delta.changes[0].root || !delta.changes[0].remote_path.empty() ||
        !delta.changes[1].directory ||
        !delta.changes[1].etag.empty() ||
        delta.changes[1].remote_path != "Documents" ||
        delta.changes[2].remote_path != "Documents/notes.txt" ||
        delta.changes[2].parent_id != "folder-id" ||
        delta.changes[2].size != 42 ||
        !delta.changes[2].content_hash ||
        delta.changes[2].content_hash->algorithm !=
            onedrive::FileHashAlgorithm::sha256 ||
        delta.changes[2].content_hash->value !=
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" ||
        !delta.changes[3].deleted ||
        progress !=
            std::vector<std::tuple<std::size_t, std::size_t, bool>>{
                {1, 2, false},
                {2, 4, true},
            }) {
        return fail("Graph delta items or final link were not parsed");
    }
    if (transport_pointer->requests.size() != 3 ||
        transport_pointer->requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/delta" ||
        transport_pointer->requests[2].url !=
            "https://graph.example.test/v1.0/delta?page=2") {
        return fail("Graph delta pagination requests were incorrect");
    }
    return EXIT_SUCCESS;
}

int test_delta_resume_and_url_validation() {
    constexpr std::string_view saved_delta_link{
        "https://graph.example.test/v1.0/delta?token=saved"
    };
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
                    R"({"value":[],"@odata.deltaLink":)"
                    R"("https://graph.example.test/v1.0/delta?token=next"})",
            },
        }
    );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    const auto delta = client.list_delta(std::string{saved_delta_link});
    if (!delta.changes.empty() ||
        delta.delta_link !=
            "https://graph.example.test/v1.0/delta?token=next" ||
        transport_pointer->requests.size() != 2 ||
        transport_pointer->requests[1].url != saved_delta_link) {
        return fail("saved Graph delta link was not resumed");
    }

    auto untrusted_transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"value":[],"@odata.deltaLink":)"
                    R"("https://attacker.example/collect"})",
            },
        }
    );
    onedrive::graph::MicrosoftGraphClient untrusted_client{
        wrap_transport(std::move(untrusted_transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    try {
        static_cast<void>(untrusted_client.list_delta(std::nullopt));
        return fail("untrusted Graph delta link was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("outside its endpoint")) {
            return fail("untrusted Graph delta link error was not reported");
        }
    }
    return EXIT_SUCCESS;
}

int test_invalid_delta_cursor_error() {
    constexpr std::string_view saved_delta_link{
        "https://graph.example.test/v1.0/delta?token=expired"
    };
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 410,
                .body =
                    R"({"error":{"code":"resyncRequired",)"
                    R"("message":"The delta token is no longer valid."}})",
            },
        }
    );
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    try {
        static_cast<void>(client.list_delta(std::string{saved_delta_link}));
        return fail("invalid saved Graph delta cursor was accepted");
    } catch (const onedrive::graph::DeltaCursorInvalidError& error) {
        if (!std::string{error.what()}.contains("delta token is no longer valid")) {
            return fail("invalid Graph delta cursor error omitted the server detail");
        }
    }
    if (transport_pointer->requests.size() != 2 ||
        transport_pointer->requests[1].url != saved_delta_link) {
        return fail("invalid Graph delta cursor request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_file_download_redirect() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value = "https://download.example.test/content",
                    },
                },
            },
        }
    );
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() / "onedrive-cpp-download-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    client.download_file(
        "item id",
        8,
        destination,
        [&](std::uint64_t downloaded, std::uint64_t total) {
            progress.emplace_back(downloaded, total);
        }
    );

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    if (contents != "download" || transport_pointer->requests.size() != 2 ||
        transport_pointer->requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/items/"
            "item%20id/content" ||
        !has_header(
            transport_pointer->requests[1],
            "Authorization: Bearer access-secret"
        ) ||
        transport_pointer->download_requests.size() != 1 ||
        transport_pointer->download_requests[0].url !=
            "https://download.example.test/content" ||
        transport_pointer->download_requests[0].headers !=
            std::vector<std::string>{"Accept: application/octet-stream"} ||
        progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{{8, 8}}) {
        return fail("Graph file download redirect was not handled safely");
    }
    return EXIT_SUCCESS;
}

int test_resumed_file_download() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value = "https://download.example.test/content",
                    },
                },
            },
        }
    );
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-resumed-download-test";
    {
        std::ofstream output{destination, std::ios::binary};
        output << "down";
    }

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(
                std::string{"existing-refresh"}
            )
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    std::vector<std::uint64_t> checkpoints;
    client.download_file(
        "item-id",
        8,
        destination,
        4,
        {},
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        }
    );
    std::vector<std::pair<std::uint64_t, std::uint64_t>> completed_progress;
    client.download_file(
        "item-id",
        8,
        destination,
        8,
        [&](std::uint64_t downloaded, std::uint64_t total) {
            completed_progress.emplace_back(downloaded, total);
        },
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        }
    );
    try {
        client.download_file("item-id", 8, destination, 9, {}, {});
        return fail("Graph download accepted a resume offset beyond file size");
    } catch (const std::invalid_argument&) {
    }

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    if (contents != "download" ||
        transport_pointer->download_requests.size() != 1 ||
        transport_pointer->download_requests[0].download_offset != 4 ||
        transport_pointer->download_requests[0].headers.back() !=
            "Range: bytes=4-7" ||
        checkpoints != std::vector<std::uint64_t>{8, 8} ||
        completed_progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{{8, 8}}) {
        return fail("Graph file download did not resume from its byte offset");
    }
    return EXIT_SUCCESS;
}

int test_large_file_chunked_download() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value = "https://download.example.test/content",
                    },
                },
            },
        }
    );
    auto* transport_pointer = transport.get();
    transport_pointer->download_responses = {
        onedrive::http::HttpResponse{.status_code = 206},
        onedrive::http::HttpResponse{
            .status_code = 503,
            .headers = {{"Retry-After", "0"}},
        },
        onedrive::http::HttpResponse{.status_code = 206},
        onedrive::http::HttpResponse{.status_code = 206},
    };
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-chunked-download-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .download_chunk_threshold_bytes = 3,
        },
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    std::vector<std::uint64_t> checkpoints;
    client.download_file(
        "item-id",
        8,
        destination,
        0,
        [&](std::uint64_t downloaded, std::uint64_t total) {
            progress.emplace_back(downloaded, total);
        },
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        }
    );

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    if (contents != "download" ||
        transport_pointer->download_requests.size() != 4 ||
        transport_pointer->download_requests[0].headers.back() !=
            "Range: bytes=0-2" ||
        transport_pointer->download_requests[1].headers.back() !=
            "Range: bytes=3-5" ||
        transport_pointer->download_requests[2].headers.back() !=
            "Range: bytes=3-5" ||
        transport_pointer->download_requests[3].headers.back() !=
            "Range: bytes=6-7" ||
        transport_pointer->download_requests[1].download_offset != 3 ||
        sleeps != std::vector{std::chrono::seconds{0}} ||
        progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {3, 8},
                {6, 8},
                {6, 8},
                {8, 8},
            } ||
        checkpoints != std::vector<std::uint64_t>{3, 6, 8}) {
        return fail("large Graph file was not downloaded in byte ranges");
    }
    return EXIT_SUCCESS;
}

int test_drive_identity_and_profile_photo() {
    FakeTransport transport{
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"user-id","displayName":"Alice Example"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"canonical-drive-id","name":"Alice Drive"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .headers = {
                    {.name = "Content-Type", .value = "image/png"},
                },
                .body = "photo-bytes",
            },
        }
    };
    onedrive::http::HttpTransport transport_proxy{
        onedrive::detail::borrowed_proxy,
        transport
    };
    const auto identity = onedrive::graph::fetch_drive_identity(
        transport_proxy,
        "access-token",
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        }
    );
    if (identity.user_id != "user-id" ||
        identity.user_display_name != "Alice Example" ||
        identity.configured_drive_id != "me" ||
        identity.drive_id != "canonical-drive-id" ||
        identity.drive_name != "Alice Drive" || !identity.photo ||
        identity.photo->content_type != "image/png" ||
        std::string{
            identity.photo->bytes.begin(),
            identity.photo->bytes.end()
        } != "photo-bytes" ||
        transport.requests.size() != 3 ||
        transport.requests[0].url !=
            "https://graph.example.test/v1.0/me?$select=id,displayName" ||
        transport.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive?$select=id,name" ||
        transport.requests[2].url !=
            "https://graph.example.test/v1.0/me/photo/$value" ||
        !has_header(transport.requests[2], "Authorization: ******")) {
        return fail("Graph account and drive identity were not loaded");
    }
    FakeTransport without_photo{
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"user-id","displayName":"Alice Example"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"canonical-drive-id","name":"Alice Drive"})",
            },
            onedrive::http::HttpResponse{.status_code = 404},
        }
    };
    onedrive::http::HttpTransport without_photo_proxy{
        onedrive::detail::borrowed_proxy,
        without_photo
    };
    if (onedrive::graph::fetch_drive_identity(
            without_photo_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        ).photo) {
        return fail("missing Graph profile photo was not treated as optional");
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
    if (const int result = test_invalid_file_hash(); result != EXIT_SUCCESS) {
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
    if (const int result = test_throttling_fallback_and_limit();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_transient_service_retries();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_delta_with_pagination();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_delta_resume_and_url_validation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_delta_cursor_error();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_file_download_redirect();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_resumed_file_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_large_file_chunked_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_drive_identity_and_profile_photo();
}
