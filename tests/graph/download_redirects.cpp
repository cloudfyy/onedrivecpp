#include "support.hpp"
#include "download_cases.hpp"

namespace onedrive::test::graph::download_cases {

using namespace onedrive::test::graph;

int test_file_download_redirect() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            redirect_response("https://download.example.test/content"),
            redirect_response("https://download.example.test/empty"),
        });
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "download";
    std::error_code ignored;

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0",
            .download_transport = {
                .transfer =
                    {
                        .connect_timeout = std::chrono::seconds{12},
                        .operation_timeout = std::chrono::seconds{600},
                        .low_speed_timeout = std::chrono::seconds{15},
                        .low_speed_limit_bytes_per_second = 128,
                        .http_version = onedrive::http::HttpVersion::http_2,
                        .ip_version = onedrive::http::IpVersion::ipv4,
                    },
                .maximum_receive_speed_bytes_per_second = 1'048'576,
                .maximum_total_receive_speed_bytes_per_second = 2'097'152,
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
                reinterpret_cast<const char*>(data.data()), data.size()
            );
        }
    );

    const auto contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    transport_pointer->download_body.clear();
    client.download_file("empty-item", "\"empty-etag\"", 0, destination);
    const bool empty_file_downloaded =
        std::filesystem::is_regular_file(destination) &&
        std::filesystem::file_size(destination) == 0;
    std::filesystem::remove(destination, ignored);
    if (transport_pointer->queued.requests.size() != 3 ||
        !has_header(
            transport_pointer->queued.requests[1], "If-Match: \"item-etag\""
        ) ||
        !has_header(
            transport_pointer->queued.requests[2], "If-Match: \"empty-etag\""
        )) {
        return fail("Graph download eTag preconditions were not sent");
    }
    if (contents != "download" || !empty_file_downloaded ||
        transport_pointer->queued.requests.size() != 3 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/items/"
            "item%20id/content" ||
        !has_header(
            transport_pointer->queued.requests[1],
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
        transport_pointer->download_requests[0]
                .low_speed_limit_bytes_per_second != 128 ||
        transport_pointer->download_requests[0]
                .maximum_receive_speed_bytes_per_second != 1'048'576 ||
        !transport_pointer->download_requests[0].download_throttle ||
        !transport_pointer->download_requests[0].download_throttle(1, {}) ||
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
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 412,
                .headers = {{"Retry-After", "0"}},
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
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .maximum_throttle_retries = 4,
        },
        [&](std::chrono::seconds duration) { sleeps.push_back(duration); },
    };
    const auto destination = test_directory() / "changed-download";
    std::error_code ignored;
    try {
        client.download_file("item-id", "W/\"expected-etag\"", 8, destination);
        return fail("changed Graph drive item was downloaded");
    } catch (const onedrive::graph::RemoteItemChangedError& error) {
        if (!std::string_view{error.what()}.contains(
                "changed before download"
            )) {
            return fail("changed Graph drive item error was not actionable");
        }
    }
    if (transport_pointer->queued.requests.size() != 2 ||
        !has_header(
            transport_pointer->queued.requests[1],
            "If-Match: W/\"expected-etag\""
        ) ||
        !transport_pointer->download_requests.empty() || !sleeps.empty() ||
        std::filesystem::exists(destination)) {
        return fail("changed Graph drive item request was retried or written");
    }

    try {
        client.download_file(
            "item-id", "\"invalid\r\nX-Injected: true\"", 8, destination
        );
        return fail("invalid Graph drive item eTag was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (transport_pointer->queued.requests.size() != 2) {
        return fail("invalid Graph drive item eTag made an HTTP request");
    }
    return EXIT_SUCCESS;
}

int test_relaxed_file_download_ignores_remote_size() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            redirect_response("https://download.example.test/protected"),
            redirect_response("https://download.example.test/protected-empty"),
        });
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "relaxed-download";
    std::error_code ignored;

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
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
        "protected-item", "\"protected-etag\"", 100, destination
    );

    const auto contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    transport_pointer->download_body.clear();
    client.download_file(
        "protected-empty", "\"protected-empty-etag\"", 0, destination
    );
    const bool empty_file_downloaded =
        std::filesystem::is_regular_file(destination) &&
        std::filesystem::file_size(destination) == 0;
    std::filesystem::remove(destination, ignored);
    if (contents != "download" || !empty_file_downloaded ||
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
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
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
        });
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
    const auto destination = test_directory() / "whole-download-gate";
    std::error_code ignored;

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
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
        [&](std::uint64_t completed) { checkpoints.push_back(completed); },
        [&](std::uint64_t offset, std::span<const std::byte>) {
            observed_offsets.push_back(offset);
        }
    );

    const auto contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    if (contents != "download" ||
        transport_pointer->download_requests.size() != 2 ||
        transport_pointer->download_response_gate_count != 2 ||
        observed_offsets != std::vector<std::uint64_t>{0} ||
        checkpoints != std::vector<std::uint64_t>{8}) {
        return fail("whole-file download persisted a rejected response body");
    }
    return EXIT_SUCCESS;
}

int test_expired_download_redirect_is_refreshed() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers =
                    {
                        {
                            .name = "Location",
                            .value = "https://download.example.test/expired",
                        },
                    },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers =
                    {
                        {
                            .name = "Location",
                            .value = "https://download.example.test/refreshed",
                        },
                    },
            },
            onedrive::http::HttpResponse{
                .status_code = 302,
                .headers =
                    {
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
        });
    transport->download_responses = {
        onedrive::http::HttpResponse{.status_code = 403},
        onedrive::http::HttpResponse{.status_code = 200},
        onedrive::http::HttpResponse{.status_code = 403},
        onedrive::http::HttpResponse{.status_code = 403},
    };
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "refreshed-redirect";
    std::error_code ignored;

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
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

    if (transport_pointer->queued.requests.size() != 5 ||
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

} // namespace onedrive::test::graph::download_cases
