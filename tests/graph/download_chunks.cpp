#include "support.hpp"
#include "download_cases.hpp"

namespace onedrive::test::graph::download_cases {

using namespace onedrive::test::graph;

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
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                redirect_response("https://download.example.test/content"),
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

} // namespace onedrive::test::graph::download_cases
