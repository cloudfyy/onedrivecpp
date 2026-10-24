#include "support.hpp"
#include "download_cases.hpp"

namespace onedrive::test::graph::download_cases {

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

int test_download_account_authentication_failure() {
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(
            std::make_unique<FakeTransport>(
                std::deque<onedrive::http::HttpResult>{
                    token_response(),
                    onedrive::http::HttpResponse{.status_code = 401},
                }
            )
        ),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {},
    };
    try {
        client.download_file(
            "item", "\"etag\"", 8, test_directory() / "unauthorized"
        );
        return fail("unauthorized Graph download was accepted");
    } catch (const onedrive::graph::AccountAuthenticationError&) {
        return EXIT_SUCCESS;
    } catch (const std::runtime_error&) {
        return fail("download authentication failure lost its account scope");
    }
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

} // namespace onedrive::test::graph::download_cases
