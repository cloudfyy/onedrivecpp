#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

int test_invalid_download_transport_options() {
    try {
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(
                std::make_unique<FakeTransport>(
                    std::deque<onedrive::http::HttpResult>{}
                )
            ),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
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
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
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
            redirect_response(
                "https://download.example.test/protected-empty"
            ),
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
int test_resumed_file_download() {
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
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "resumed-download";
    {
        std::ofstream output{destination, std::ios::binary};
        output << "down";
    }

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
        [&](std::uint64_t completed) { checkpoints.push_back(completed); },
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
        [&](std::uint64_t completed) { checkpoints.push_back(completed); }
    );
    try {
        client.download_file(
            "item-id", "\"item-etag\"", 8, destination, 9, {}, {}, {}
        );
        return fail("Graph download accepted a resume offset beyond file size");
    } catch (const std::invalid_argument&) {
    }

    const auto contents = onedrive::test::read_file(destination);
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
    transport->download_responses.push_back(
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers =
                {
                    {.name = "Content-Range", .value = "bytes 0-2/8"},
                },
            .received_size = 3,
        }
    );
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "cancelled-download";
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
            [&](std::uint64_t, std::uint64_t) { cancellation.request_stop(); },
            [&](std::uint64_t completed) { checkpoints.push_back(completed); }
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
    auto* transport_pointer = transport.get();
    transport_pointer->download_responses = {
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers =
                {
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
            .headers =
                {
                    {.name = "content-range", .value = "bytes 3-5/8"},
                },
            .received_size = 3,
        },
        onedrive::http::HttpResponse{
            .status_code = 206,
            .headers =
                {
                    {.name = "Content-Range", .value = "bytes 6-7/8"},
                },
            .received_size = 2,
        },
    };
    const auto destination = test_directory() / "chunked-download";
    std::error_code ignored;

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
        [&](std::uint64_t completed) { checkpoints.push_back(completed); }
    );

    const auto contents = onedrive::test::read_file(destination);
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
        transport_pointer->download_requests[0]
                .download_checkpoint_interval_bytes != 2 ||
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
    transport->download_responses.push_back(
        std::unexpected(
            onedrive::http::HttpError{
                .message = "simulated interrupted range",
            }
        )
    );
    transport->partial_failure_bytes = 1;
    auto* transport_pointer = transport.get();
    const auto destination = test_directory() / "durable-range-retry";
    std::error_code ignored;

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
            .download_chunk_threshold_bytes = 3,
        },
        [&](std::chrono::seconds duration) { sleeps.push_back(duration); },
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
        [&](std::uint64_t completed) { checkpoints.push_back(completed); }
    );

    const auto contents = onedrive::test::read_file(destination);
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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
                .headers =
                    {
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

    const auto destination = test_directory() / "invalid-chunk-response";
    std::error_code ignored;
    for (auto& [response, expected_message] : cases) {
        std::filesystem::remove(destination, ignored);
        auto transport = std::make_unique<
            FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            redirect_response("https://download.example.test/content"),
        });
        auto* transport_pointer = transport.get();
        transport->download_responses.push_back(std::move(response));
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
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
            client.download_file("item-id", "\"item-etag\"", 8, destination);
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
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            redirect_response("https://download.example.test/content"),
        });
    auto* transport_pointer = transport.get();
    transport->download_responses = {
        HttpResponse{
            .status_code = 206,
            .headers =
                {
                    {.name = "Content-Range", .value = "bytes 0-2/8"},
                },
            .received_size = 3,
        },
        HttpResponse{
            .status_code = 206,
            .headers =
                {
                    {.name = "Content-Range", .value = "bytes 4-6/8"},
                },
            .received_size = 3,
        },
    };
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
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
            [&](std::uint64_t completed) { checkpoints.push_back(completed); }
        );
        std::filesystem::remove(destination, ignored);
        return fail("invalid later Graph chunk response was accepted");
    } catch (const std::runtime_error&) {
    }
    const auto partial_contents = onedrive::test::read_file(destination);
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

} // namespace

int main() {
    if (const int result = test_invalid_download_transport_options();
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
    if (const int result = test_chunk_retry_resumes_from_durable_checkpoint();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_chunk_responses_are_rejected();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
