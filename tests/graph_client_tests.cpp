#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <format>
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
        const onedrive::http::DownloadProgress& progress,
        const onedrive::http::DownloadData& data,
        const onedrive::http::DownloadCheckpoint& checkpoint,
        const onedrive::http::DownloadResponseGate& response_gate
    ) const {
        download_requests.push_back(request);
        if (response_gate) {
            ++download_response_gate_count;
        }
        std::string body = download_body;
        long status_code = 200;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
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
            range = {start, end};
        }
        onedrive::http::HttpResponse response{
            .status_code = status_code,
            .received_size = static_cast<std::uint64_t>(body.size()),
        };
        if (range.has_value()) {
            response.headers.push_back({
                .name = "Content-Range",
                .value = std::format(
                    "bytes {}-{}/{}",
                    range->first,
                    range->second,
                    download_body.size()
                ),
            });
        }
        std::optional<onedrive::http::HttpError> transfer_error;
        if (!download_responses.empty()) {
            auto configured = std::move(download_responses.front());
            download_responses.pop_front();
            if (!configured) {
                if (partial_failure_bytes == 0) {
                    return configured;
                }
                const auto retained = std::min(
                    partial_failure_bytes,
                    static_cast<std::uint64_t>(body.size())
                );
                body.resize(static_cast<std::size_t>(retained));
                response.received_size = retained;
                transfer_error = configured.error();
                partial_failure_bytes = 0;
            } else {
                response = std::move(*configured);
            }
        }
        const bool accepted =
            !response_gate ||
            response_gate(response.status_code, response.headers);
        if (accepted && request.download_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << body;
        } else if (accepted) {
            std::fstream output{
                destination,
                std::ios::binary | std::ios::in | std::ios::out
            };
            output.seekp(static_cast<std::streamoff>(request.download_offset));
            output << body;
        }
        if (accepted && data) {
            data(
                request.download_offset,
                std::as_bytes(std::span{body})
            );
        }
        if (accepted && progress) {
            progress(body.size(), body.size());
        }
        if (request.stop_token.stop_requested()) {
            return std::unexpected(onedrive::http::HttpError{
                .code = onedrive::http::HttpErrorCode::cancelled,
                .message = "fake download was cancelled",
            });
        }
        if (accepted && checkpoint &&
            response.status_code >= 200 && response.status_code < 300) {
            checkpoint(
                request.download_offset +
                static_cast<std::uint64_t>(body.size())
            );
        }
        if (transfer_error.has_value()) {
            return std::unexpected(std::move(*transfer_error));
        }
        return response;
    }

    mutable std::vector<onedrive::http::HttpRequest> requests;
    mutable std::vector<onedrive::http::HttpRequest> download_requests;
    mutable std::deque<onedrive::http::HttpResult> download_responses;
    mutable std::size_t download_response_gate_count{0};
    mutable std::uint64_t partial_failure_bytes{0};
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

int test_invalid_download_transport_options() {
    try {
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(
                std::make_unique<FakeTransport>(
                    std::deque<onedrive::http::HttpResult>{}
                )
            ),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .download_transport = {
                    .transfer = {
                        .connect_timeout = std::chrono::seconds::zero(),
                    },
                },
            },
        };
        static_cast<void>(client);
        return fail("invalid Graph download transport options were accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(
                std::make_unique<FakeTransport>(
                    std::deque<onedrive::http::HttpResult>{}
                )
            ),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .download_checkpoint_interval_bytes = 0,
            },
        };
        static_cast<void>(client);
        return fail("zero Graph download checkpoint interval was accepted");
    } catch (const std::invalid_argument&) {
    }
    return EXIT_SUCCESS;
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
                    R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","malware":{},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA="}}}]})json",
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
        !items[1].malware ||
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

int test_item_lookup_by_encoded_path() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"existing-refresh"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"json({"id":"file-id","name":"report #1.txt","eTag":"file-etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T00:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder A"},"file":{"hashes":{"sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}})json",
            },
        }
    );
    auto* transport_pointer = transport.get();
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

    const auto item = client.item_by_path("Folder A/report #1.txt");
    if (item.id != "file-id" || item.name != "report #1.txt" ||
        item.remote_path != "Folder A/report #1.txt" ||
        item.parent_id != "folder-id" || item.size != 4 ||
        item.directory || item.last_modified != "2026-10-04T00:00:00Z" ||
        !item.content_hash ||
        item.content_hash->algorithm !=
            onedrive::FileHashAlgorithm::sha256 ||
        transport_pointer->requests.size() != 2 ||
        transport_pointer->requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/root:/Folder%20A/"
            "report%20%231.txt?$select=id,name,eTag,size,fileSystemInfo,"
            "parentReference,file,folder,deleted,malware,remoteItem" ||
        !has_header(
            transport_pointer->requests[1],
            "Authorization: ******"
        )) {
        return fail("Graph path lookup was not encoded or parsed correctly");
    }

    try {
        static_cast<void>(client.item_by_path("../unsafe.txt"));
        return fail("unsafe Graph path lookup was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (transport_pointer->requests.size() != 2) {
        return fail("unsafe Graph path lookup made an HTTP request");
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
        }
    );
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})),
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
        transport_pointer->requests.size() != 6) {
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
                    R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","size":42,"lastModifiedDateTime":"2026-10-02T00:01:00Z","fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T23:59:58.123456789Z"},"parentReference":{"id":"folder-id","path":"/drives/drive-id/root:/Documents"},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA=","sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}},{"id":"shortcut-id","name":"shared.txt","eTag":"shortcut-etag","size":7,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T20:00:00Z"},"remoteItem":{"fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T21:00:00Z"},"malware":{}},"parentReference":{"id":"folder-id","path":"/drives/drive-id/root:/Documents"},"file":{"mimeType":"text/plain"}},{"id":"deleted-id","deleted":{"state":"deleted"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?token=final"})json",
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
            .relaxed_download_validation = true,
        },
    };

    std::vector<std::tuple<std::size_t, std::size_t, bool>> progress;
    const auto delta = client.list_delta(
        std::nullopt,
        [&progress](std::size_t pages, std::size_t items, bool completed) {
            progress.emplace_back(pages, items, completed);
        }
    );
    if (delta.changes.size() != 5 ||
        delta.delta_link !=
            "https://graph.example.test/v1.0/delta?token=final" ||
        !delta.changes[0].root || !delta.changes[0].remote_path.empty() ||
        !delta.changes[1].directory ||
        !delta.changes[1].etag.empty() ||
        delta.changes[1].remote_path != "Documents" ||
        delta.changes[2].remote_path != "Documents/notes.txt" ||
        delta.changes[2].parent_id != "folder-id" ||
        delta.changes[2].size != 42 ||
        delta.changes[2].last_modified !=
            "2026-10-01T23:59:58.123456789Z" ||
        !delta.changes[2].content_hash ||
        delta.changes[2].content_hash->algorithm !=
            onedrive::FileHashAlgorithm::sha256 ||
        delta.changes[2].content_hash->value !=
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" ||
        delta.changes[2].validate_content ||
        delta.changes[3].last_modified != "2026-10-01T21:00:00Z" ||
        !delta.changes[3].malware ||
        !delta.changes[4].deleted ||
        progress !=
            std::vector<std::tuple<std::size_t, std::size_t, bool>>{
                {1, 2, false},
                {2, 5, true},
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

int test_delta_requires_valid_file_system_modified_time() {
    const auto rejected = [](std::string body, std::string_view message) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"expires_in":3600,"access_token":"access-secret"})",
                },
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body = std::move(body),
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
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            static_cast<void>(client.list_delta(std::nullopt));
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.contains(message);
        }
        return false;
    };

    constexpr std::string_view missing_time{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{missing_time},
            "missing fileSystemInfo.lastModifiedDateTime"
        )) {
        return fail("Graph delta file without authoritative mtime was accepted");
    }

    constexpr std::string_view invalid_time{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-02-30T00:00:00Z"},"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_time},
            "invalid Microsoft Graph modification time"
        )) {
        return fail("Graph delta file with invalid authoritative mtime was accepted");
    }

    constexpr std::string_view invalid_malware{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-02T00:00:00Z"},"malware":true,"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_malware},
            "invalid drive item malware facet"
        )) {
        return fail("Graph delta file with invalid malware metadata was accepted");
    }

    constexpr std::string_view invalid_remote_malware{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-02T00:00:00Z"},"remoteItem":{"malware":true},"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_remote_malware},
            "invalid remoteItem malware facet"
        )) {
        return fail(
            "Graph delta file with invalid remote malware metadata was accepted"
        );
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
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value = "https://download.example.test/empty",
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
            .download_transport = {
                .transfer = {
                    .connect_timeout = std::chrono::seconds{12},
                    .operation_timeout = std::chrono::seconds{600},
                    .low_speed_timeout = std::chrono::seconds{15},
                    .low_speed_limit_bytes_per_second = 128,
                    .http_version = onedrive::http::HttpVersion::http_2,
                    .ip_version = onedrive::http::IpVersion::ipv4,
                },
                .maximum_receive_speed_bytes_per_second = 1'048'576,
                .maximum_total_receive_speed_bytes_per_second =
                    2'097'152,
            },
        },
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    std::string observed_download_data;
    std::vector<std::uint64_t> observed_download_offsets;
    client.download_file(
        "item id",
        "\"item-etag\"",
        8,
        destination,
        0,
        {},
        [&](std::uint64_t downloaded, std::uint64_t total) {
            progress.emplace_back(downloaded, total);
        },
        {},
        [&](std::uint64_t offset, std::span<const std::byte> data) {
            observed_download_offsets.push_back(offset);
            observed_download_data.append(
                reinterpret_cast<const char*>(data.data()),
                data.size()
            );
        }
    );

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    transport_pointer->download_body.clear();
    client.download_file("empty-item", "\"empty-etag\"", 0, destination);
    const bool empty_file_downloaded =
        std::filesystem::is_regular_file(destination) &&
        std::filesystem::file_size(destination) == 0;
    std::filesystem::remove(destination, ignored);
    if (transport_pointer->requests.size() != 3 ||
        !has_header(
            transport_pointer->requests[1],
            "If-Match: \"item-etag\""
        ) ||
        !has_header(
            transport_pointer->requests[2],
            "If-Match: \"empty-etag\""
        )) {
        return fail("Graph download eTag preconditions were not sent");
    }
    if (contents != "download" || !empty_file_downloaded ||
        transport_pointer->requests.size() != 3 ||
        transport_pointer->requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/items/"
            "item%20id/content" ||
        !has_header(
            transport_pointer->requests[1],
            "Authorization: Bearer access-secret"
        ) ||
        transport_pointer->download_requests.size() != 2 ||
        !transport_pointer->download_requests[0].follow_redirects ||
        transport_pointer->download_requests[0].maximum_redirects != 5 ||
        !transport_pointer->download_requests[1].follow_redirects ||
        transport_pointer->download_requests[1].maximum_redirects != 5 ||
        transport_pointer->download_requests[0].url !=
            "https://download.example.test/content" ||
        transport_pointer->download_requests[0].headers !=
            std::vector<std::string>{"Accept: application/octet-stream"} ||
        transport_pointer->download_requests[0].connect_timeout !=
            std::chrono::seconds{12} ||
        transport_pointer->download_requests[0].operation_timeout !=
            std::chrono::seconds{600} ||
        transport_pointer->download_requests[0].low_speed_timeout !=
            std::chrono::seconds{15} ||
        transport_pointer->download_requests[0].
                low_speed_limit_bytes_per_second !=
            128 ||
        transport_pointer->download_requests[0].
                maximum_receive_speed_bytes_per_second !=
            1'048'576 ||
        !transport_pointer->download_requests[0].
            download_throttle ||
        !transport_pointer->download_requests[0].
            download_throttle(1, {}) ||
        transport_pointer->download_requests[0].http_version !=
            onedrive::http::HttpVersion::http_2 ||
        transport_pointer->download_requests[0].ip_version !=
            onedrive::http::IpVersion::ipv4 ||
        transport_pointer->download_requests[1].url !=
            "https://download.example.test/empty" ||
        transport_pointer->download_response_gate_count != 2 ||
        observed_download_offsets != std::vector<std::uint64_t>{0} ||
        observed_download_data != "download" ||
        progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{{8, 8}}) {
        return fail("Graph file download redirect was not handled safely");
    }
    return EXIT_SUCCESS;
}

int test_changed_file_download_is_not_retried() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 412,
                .headers = {{"Retry-After", "0"}},
            },
        }
    );
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
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
            .maximum_throttle_retries = 4,
        },
        [&](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-changed-download-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    try {
        client.download_file(
            "item-id",
            "W/\"expected-etag\"",
            8,
            destination
        );
        return fail("changed Graph drive item was downloaded");
    } catch (const onedrive::graph::RemoteItemChangedError& error) {
        if (!std::string_view{error.what()}.contains(
                "changed before download"
            )) {
            return fail("changed Graph drive item error was not actionable");
        }
    }
    if (transport_pointer->requests.size() != 2 ||
        !has_header(
            transport_pointer->requests[1],
            "If-Match: W/\"expected-etag\""
        ) ||
        !transport_pointer->download_requests.empty() ||
        !sleeps.empty() ||
        std::filesystem::exists(destination)) {
        return fail("changed Graph drive item request was retried or written");
    }

    try {
        client.download_file(
            "item-id",
            "\"invalid\r\nX-Injected: true\"",
            8,
            destination
        );
        return fail("invalid Graph drive item eTag was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (transport_pointer->requests.size() != 2) {
        return fail("invalid Graph drive item eTag made an HTTP request");
    }
    return EXIT_SUCCESS;
}

int test_relaxed_file_download_ignores_remote_size() {
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
                        .value = "https://download.example.test/protected",
                    },
                },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value =
                            "https://download.example.test/protected-empty",
                    },
                },
            },
        }
    );
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-relaxed-download-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

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
            .download_chunk_threshold_bytes = 4,
            .relaxed_download_validation = true,
        },
    };
    client.download_file(
        "protected-item",
        "\"protected-etag\"",
        100,
        destination
    );

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    transport_pointer->download_body.clear();
    client.download_file(
        "protected-empty",
        "\"protected-empty-etag\"",
        0,
        destination
    );
    const bool empty_file_downloaded =
        std::filesystem::is_regular_file(destination) &&
        std::filesystem::file_size(destination) == 0;
    std::filesystem::remove(destination, ignored);
    if (contents != "download" ||
        !empty_file_downloaded ||
        transport_pointer->download_requests.size() != 2 ||
        transport_pointer->download_response_gate_count != 2 ||
        transport_pointer->download_requests[0].headers !=
            std::vector<std::string>{"Accept: application/octet-stream"} ||
        transport_pointer->download_requests[0].download_offset != 0) {
        return fail(
            "relaxed Graph download relied on remote size or chunk ranges"
        );
    }
    try {
        client.download_file(
            "protected-item",
            "\"protected-etag\"",
            100,
            destination,
            1,
            {},
            {},
            {}
        );
        return fail("relaxed Graph download accepted a resume offset");
    } catch (const std::invalid_argument&) {
    }
    return EXIT_SUCCESS;
}

int test_whole_file_download_rejects_error_body_before_retry() {
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
    transport->download_responses = {
        onedrive::http::HttpResponse{
            .status_code = 503,
            .headers = {{"Retry-After", "0"}},
            .received_size = 8,
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .received_size = 8,
        },
    };
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-whole-download-gate-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

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
            .download_maximum_retries = 1,
        },
    };
    std::vector<std::uint64_t> observed_offsets;
    std::vector<std::uint64_t> checkpoints;
    client.download_file(
        "item-id",
        "\"item-etag\"",
        8,
        destination,
        0,
        {},
        {},
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        },
        [&](std::uint64_t offset, std::span<const std::byte>) {
            observed_offsets.push_back(offset);
        }
    );

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    if (contents != "download" ||
        transport_pointer->download_requests.size() != 2 ||
        transport_pointer->download_response_gate_count != 2 ||
        observed_offsets != std::vector<std::uint64_t>{0} ||
        checkpoints != std::vector<std::uint64_t>{8}) {
        return fail(
            "whole-file download persisted a rejected response body"
        );
    }
    return EXIT_SUCCESS;
}

int test_expired_download_redirect_is_refreshed() {
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
                        .value = "https://download.example.test/expired",
                    },
                },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value = "https://download.example.test/refreshed",
                    },
                },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value =
                            "https://download.example.test/expired-again",
                    },
                },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers = {
                    {
                        .name = "Location",
                        .value =
                            "https://download.example.test/refreshed-again",
                    },
                },
            },
        }
    );
    transport->download_responses = {
        onedrive::http::HttpResponse{.status_code = 403},
        onedrive::http::HttpResponse{.status_code = 200},
        onedrive::http::HttpResponse{.status_code = 403},
        onedrive::http::HttpResponse{.status_code = 403},
    };
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-refreshed-redirect-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

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
    client.download_file("item-id", "\"item-etag\"", 8, destination);
    std::filesystem::remove(destination, ignored);
    try {
        client.download_file("item-id", "\"item-etag\"", 8, destination);
        std::filesystem::remove(destination, ignored);
        return fail("persistently expired download redirect was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains("HTTP 403")) {
            std::filesystem::remove(destination, ignored);
            return fail("persistent expired redirect error was not reported");
        }
    }
    std::filesystem::remove(destination, ignored);

    if (transport_pointer->requests.size() != 5 ||
        transport_pointer->download_requests.size() != 4 ||
        transport_pointer->download_requests[0].url !=
            "https://download.example.test/expired" ||
        transport_pointer->download_requests[1].url !=
            "https://download.example.test/refreshed" ||
        transport_pointer->download_requests[2].url !=
            "https://download.example.test/expired-again" ||
        transport_pointer->download_requests[3].url !=
            "https://download.example.test/refreshed-again" ||
        std::ranges::any_of(
            transport_pointer->download_requests,
            [](const onedrive::http::HttpRequest& request) {
                return has_header(request, "Authorization: ******");
            }
        )) {
        return fail("expired Graph download redirect was not refreshed safely");
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
    std::vector<std::uint64_t> observed_offsets;
    client.download_file(
        "item-id",
        "\"item-etag\"",
        8,
        destination,
        4,
        {},
        {},
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        },
        [&](std::uint64_t offset, std::span<const std::byte>) {
            observed_offsets.push_back(offset);
        }
    );
    std::vector<std::pair<std::uint64_t, std::uint64_t>> completed_progress;
    client.download_file(
        "item-id",
        "\"item-etag\"",
        8,
        destination,
        8,
        {},
        [&](std::uint64_t downloaded, std::uint64_t total) {
            completed_progress.emplace_back(downloaded, total);
        },
        [&](std::uint64_t completed) {
            checkpoints.push_back(completed);
        }
    );
    try {
        client.download_file(
            "item-id",
            "\"item-etag\"",
            8,
            destination,
            9,
            {},
            {},
            {}
        );
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
        observed_offsets != std::vector<std::uint64_t>{4} ||
        checkpoints != std::vector<std::uint64_t>{8, 8} ||
        completed_progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{{8, 8}}) {
        return fail("Graph file download did not resume from its byte offset");
    }
    return EXIT_SUCCESS;
}

int test_cancelled_download_is_not_retried_or_checkpointed() {
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
    transport->download_responses.push_back(
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "Content-Range", .value = "bytes 0-2/8"},
            },
            .received_size = 3,
        }
    );
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-cancelled-download-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

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
            .download_chunk_threshold_bytes = 3,
        },
    };
    std::vector<std::uint64_t> checkpoints;
    std::stop_source cancellation;
    try {
        client.download_file(
            "item-id",
            "\"item-etag\"",
            8,
            destination,
            0,
            cancellation.get_token(),
            [&](std::uint64_t, std::uint64_t) {
                cancellation.request_stop();
            },
            [&](std::uint64_t completed) {
                checkpoints.push_back(completed);
            }
        );
        std::filesystem::remove(destination, ignored);
        return fail("Graph download ignored transport cancellation");
    } catch (const onedrive::graph::DownloadCancelledError&) {
    }
    std::filesystem::remove(destination, ignored);

    if (transport_pointer->download_requests.size() != 1 ||
        !checkpoints.empty()) {
        return fail("Graph cancellation was retried or checkpointed");
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
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "Content-Range", .value = "bytes 0-2/8"},
            },
            .received_size = 3,
        },
        onedrive::http::HttpResponse{
            .status_code = 503,
            .headers = {{"Retry-After", "0"}},
        },
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "content-range", .value = "bytes 3-5/8"},
            },
            .received_size = 3,
        },
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "Content-Range", .value = "bytes 6-7/8"},
            },
            .received_size = 2,
        },
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
            .maximum_throttle_retries = 0,
            .download_maximum_retries = 1,
            .download_chunk_threshold_bytes = 3,
            .download_checkpoint_interval_bytes = 2,
        },
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    std::vector<std::uint64_t> checkpoints;
    client.download_file(
        "item-id",
        "\"item-etag\"",
        8,
        destination,
        0,
        {},
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
        transport_pointer->download_requests[0].
                download_checkpoint_interval_bytes != 2 ||
        sleeps != std::vector{std::chrono::seconds{0}} ||
        progress !=
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {3, 8},
                {6, 8},
                {8, 8},
            } ||
        checkpoints != std::vector<std::uint64_t>{3, 6, 8}) {
        return fail("large Graph file was not downloaded in byte ranges");
    }
    return EXIT_SUCCESS;
}

int test_chunk_retry_resumes_from_durable_checkpoint() {
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
    transport->download_responses.push_back(
        std::unexpected(onedrive::http::HttpError{
            .message = "simulated interrupted range",
        })
    );
    transport->partial_failure_bytes = 1;
    auto* transport_pointer = transport.get();
    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-durable-range-retry-test";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

    std::vector<std::chrono::seconds> sleeps;
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
            .download_chunk_threshold_bytes = 3,
        },
        [&](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
    };
    std::vector<std::uint64_t> checkpoints;
    client.download_file(
        "item-id",
        "\"item-etag\"",
        8,
        destination,
        0,
        {},
        {},
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
            "Range: bytes=1-2" ||
        transport_pointer->download_requests[1].download_offset != 1 ||
        checkpoints != std::vector<std::uint64_t>{1, 3, 6, 8} ||
        sleeps != std::vector{std::chrono::seconds{1}}) {
        return fail(
            "interrupted Graph chunk did not resume from its durable checkpoint"
        );
    }
    return EXIT_SUCCESS;
}

int test_invalid_chunk_responses_are_rejected() {
    using onedrive::http::HttpHeader;
    using onedrive::http::HttpResponse;
    std::vector<std::pair<HttpResponse, std::string_view>> cases{
        {
            HttpResponse{.status_code = 200, .received_size = 3},
            "expected HTTP 206",
        },
        {
            HttpResponse{.status_code = 206, .received_size = 3},
            "did not return Content-Range",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes */8",
                    },
                },
                .received_size = 3,
            },
            "invalid Content-Range",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 2-1/8",
                    },
                },
                .received_size = 3,
            },
            "invalid Content-Range",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value =
                            "bytes 18446744073709551616-"
                            "18446744073709551617/18446744073709551618",
                    },
                },
                .received_size = 3,
            },
            "invalid Content-Range",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 1-3/8",
                    },
                },
                .received_size = 3,
            },
            "expected 'bytes 0-2/8'",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 0-1/8",
                    },
                },
                .received_size = 2,
            },
            "expected 'bytes 0-2/8'",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 0-2/9",
                    },
                },
                .received_size = 3,
            },
            "expected 'bytes 0-2/8'",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 0-2/8",
                    },
                    HttpHeader{
                        .name = "content-range",
                        .value = "bytes 0-2/8",
                    },
                },
                .received_size = 3,
            },
            "multiple Content-Range",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 0-2/8",
                    },
                },
                .received_size = 2,
            },
            "wrote 2 bytes; expected 3",
        },
        {
            HttpResponse{
                .status_code = 206,
                .headers = {
                    HttpHeader{
                        .name = "Content-Range",
                        .value = "bytes 0-2/8",
                    },
                },
                .received_size = 4,
            },
            "wrote 4 bytes; expected 3",
        },
    };

    const auto destination =
        std::filesystem::temp_directory_path() /
        "onedrive-cpp-invalid-chunk-response-test";
    std::error_code ignored;
    for (auto& [response, expected_message] : cases) {
        std::filesystem::remove(destination, ignored);
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"expires_in":3600,"access_token":"access-secret"})",
                },
                HttpResponse{
                    .status_code = 302,
                    .headers = {
                        {
                            .name = "Location",
                            .value =
                                "https://download.example.test/content",
                        },
                    },
                },
            }
        );
        auto* transport_pointer = transport.get();
        transport->download_responses.push_back(std::move(response));
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
                .download_chunk_threshold_bytes = 3,
            },
        };
        try {
            client.download_file(
                "item-id",
                "\"item-etag\"",
                8,
                destination
            );
            std::filesystem::remove(destination, ignored);
            return fail("invalid Graph chunk response was accepted");
        } catch (const std::runtime_error& error) {
            if (!std::string_view{error.what()}.contains(expected_message)) {
                std::filesystem::remove(destination, ignored);
                return fail(
                    "invalid Graph chunk response reported the wrong error"
                );
            }
        }
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(destination, ignored);
            return fail("rejected first Graph chunk was not rolled back");
        }
        if (transport_pointer->download_requests.size() != 1) {
            return fail("invalid Graph chunk response was retried");
        }
    }

    std::filesystem::remove(destination, ignored);
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            HttpResponse{
                .status_code = 200,
                .body =
                    R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            HttpResponse{
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
    transport->download_responses = {
        HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "Content-Range", .value = "bytes 0-2/8"},
            },
            .received_size = 3,
        },
        HttpResponse{
            .status_code = 206,
            .headers = {
                {.name = "Content-Range", .value = "bytes 4-6/8"},
            },
            .received_size = 3,
        },
    };
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
            .download_chunk_threshold_bytes = 3,
        },
    };
    std::vector<std::uint64_t> checkpoints;
    try {
        client.download_file(
            "item-id",
            "\"item-etag\"",
            8,
            destination,
            0,
            {},
            {},
            [&](std::uint64_t completed) {
                checkpoints.push_back(completed);
            }
        );
        std::filesystem::remove(destination, ignored);
        return fail("invalid later Graph chunk response was accepted");
    } catch (const std::runtime_error&) {
    }
    std::ifstream partial{destination, std::ios::binary};
    const std::string partial_contents{
        std::istreambuf_iterator<char>{partial},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    if (partial_contents != "dow" ||
        checkpoints != std::vector<std::uint64_t>{3} ||
        transport_pointer->download_requests.size() != 2) {
        return fail(
            "rejected later Graph chunk did not preserve its durable offset"
        );
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
    if (const int result = test_invalid_download_transport_options();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_lookup_by_encoded_path();
        result != EXIT_SUCCESS) {
        return result;
    }
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
    if (const int result = test_delta_requires_valid_file_system_modified_time();
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
    if (const int result = test_changed_file_download_is_not_retried();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_relaxed_file_download_ignores_remote_size();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_whole_file_download_rejects_error_body_before_retry();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_expired_download_redirect_is_refreshed();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_resumed_file_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_cancelled_download_is_not_retried_or_checkpointed();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_large_file_chunked_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_chunk_retry_resumes_from_durable_checkpoint();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_chunk_responses_are_rejected();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_drive_identity_and_profile_photo();
}
