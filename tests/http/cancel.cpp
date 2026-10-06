#include "support.hpp"
#include "onedrive/http/http_client.hpp"
#include "support/network.hpp"
#include "support/common.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <poll.h>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using onedrive::test::fail;
using onedrive::test::Socket;
using onedrive::test::http::ScopedUmask;

} // namespace

int main() {
    const ScopedUmask download_umask{0022};
    const onedrive::test::TemporaryDirectory temporary;
    std::error_code ignored;
    auto http_listener = onedrive::test::create_loopback_listener(4);
    auto& listener = http_listener.socket;
    if (listener.get() == -1) {
        return fail("cannot create HTTP test socket");
    }
    const auto port = http_listener.port;
    onedrive::http::CurlHttpClient client;
    std::string server_error;
    const auto destination = temporary.path() / "download";
    {
        std::ofstream partial{destination, std::ios::binary};
        partial << "prefix-existing";
    }
    server_error.clear();
    std::jthread failed_chunk_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for failed chunk request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        std::string request;
        char buffer[4096];
        while (!request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read failed chunk request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response{
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 5\r\n"
            "Connection: close\r\n"
            "\r\n"
            "error"
        };
        if (::send(
                connection.get(), response.data(), response.size(), MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response.size())) {
            server_error = "cannot send failed chunk response";
        }
    }};
    bool failed_response_inspected = false;
    std::vector<std::uint64_t> failed_response_checkpoints;
    const auto failed_chunk_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/chunk",
            .headers = {"Range: bytes=6-10"},
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_offset = 6,
        },
        destination,
        {},
        {},
        [&](std::uint64_t completed) {
            failed_response_checkpoints.push_back(completed);
        },
        [&](long status_code, std::span<const onedrive::http::HttpHeader>) {
            failed_response_inspected = true;
            return status_code == 206;
        }
    );
    failed_chunk_server.join();
    const auto partial_contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!failed_chunk_response || failed_chunk_response->status_code != 503 ||
        partial_contents != "prefix" || !failed_response_inspected ||
        !failed_response_checkpoints.empty()) {
        return fail("rejected HTTP chunk wrote or checkpointed response data");
    }

    {
        std::ofstream resumable{destination, std::ios::binary};
        resumable << "prefix";
    }
    server_error.clear();
    std::jthread interrupted_chunk_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for interrupted chunk request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        std::string request;
        char buffer[4096];
        while (!request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read interrupted chunk request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        const std::string header{"HTTP/1.1 206 Partial Content\r\n"
                                 "Content-Range: bytes 6-32773/32774\r\n"
                                 "Content-Length: 32768\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"};
        const std::string body(20'000, 'x');
        if (::send(
                connection.get(), header.data(), header.size(), MSG_NOSIGNAL
            ) != static_cast<ssize_t>(header.size()) ||
            ::send(connection.get(), body.data(), body.size(), MSG_NOSIGNAL) !=
                static_cast<ssize_t>(body.size())) {
            server_error = "cannot send interrupted chunk response";
        }
    }};
    std::vector<std::uint64_t> durable_checkpoints;
    const auto interrupted_chunk_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/interrupted",
            .headers = {"Range: bytes=6-32773"},
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_offset = 6,
            .download_checkpoint_interval_bytes = 4096,
        },
        destination,
        {},
        {},
        [&](std::uint64_t completed) {
            durable_checkpoints.push_back(completed);
        },
        [](long status_code,
           std::span<const onedrive::http::HttpHeader> headers) {
            return status_code == 206 &&
                   std::ranges::any_of(
                       headers,
                       [](const onedrive::http::HttpHeader& header) {
                           return header.name == "Content-Range" &&
                                  header.value == "bytes 6-32773/32774";
                       }
                   );
        }
    );
    interrupted_chunk_server.join();
    const auto interrupted_size = std::filesystem::file_size(destination);
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (interrupted_chunk_response || durable_checkpoints.empty() ||
        interrupted_size != durable_checkpoints.back() ||
        interrupted_size <= 6) {
        return fail(
            "interrupted HTTP chunk was not retained at its durable checkpoint"
        );
    }

    {
        std::ofstream resumable{destination, std::ios::binary};
        resumable << "prefix";
    }
    server_error.clear();
    std::jthread cancelled_chunk_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for cancelled chunk request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        std::string request;
        char buffer[4096];
        while (!request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read cancelled chunk request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response_start{
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Length: 10\r\n"
            "Connection: close\r\n"
            "\r\n"
            "chunk"
        };
        if (::send(
                connection.get(),
                response_start.data(),
                response_start.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response_start.size())) {
            server_error = "cannot start cancelled chunk response";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        constexpr std::string_view remainder{"-tail"};
        static_cast<void>(::send(
            connection.get(), remainder.data(), remainder.size(), MSG_NOSIGNAL
        ));
    }};
    std::stop_source cancellation;
    const auto cancelled_chunk_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/cancel",
            .headers = {"Range: bytes=6-15"},
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_offset = 6,
            .stop_token = cancellation.get_token(),
        },
        destination,
        [&](std::uint64_t downloaded, std::uint64_t) {
            if (downloaded >= 5) {
                cancellation.request_stop();
            }
        }
    );
    cancelled_chunk_server.join();
    const auto cancelled_contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (cancelled_chunk_response ||
        cancelled_chunk_response.error().code !=
            onedrive::http::HttpErrorCode::cancelled ||
        cancelled_contents != "prefix") {
        return fail("cancelled HTTP chunk did not roll back to its offset");
    }

    {
        std::ofstream resumable{destination, std::ios::binary};
        resumable << "prefix";
    }
    server_error.clear();
    std::jthread checkpointed_cancel_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error =
                "timed out waiting for checkpointed cancellation request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        std::string request;
        char buffer[4096];
        while (!request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read checkpointed cancellation request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response_start{
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Range: bytes 6-15/16\r\n"
            "Content-Length: 10\r\n"
            "Connection: close\r\n"
            "\r\n"
            "chunk"
        };
        if (::send(
                connection.get(),
                response_start.data(),
                response_start.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response_start.size())) {
            server_error = "cannot start checkpointed cancellation response";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        constexpr std::string_view remainder{"-tail"};
        static_cast<void>(::send(
            connection.get(), remainder.data(), remainder.size(), MSG_NOSIGNAL
        ));
    }};
    std::stop_source checkpointed_cancellation;
    std::vector<std::uint64_t> cancellation_checkpoints;
    const auto checkpointed_cancel_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) +
                   "/checkpointed-cancel",
            .headers = {"Range: bytes=6-15"},
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_offset = 6,
            .stop_token = checkpointed_cancellation.get_token(),
        },
        destination,
        [&](std::uint64_t downloaded, std::uint64_t) {
            if (downloaded >= 5) {
                checkpointed_cancellation.request_stop();
            }
        },
        {},
        [&](std::uint64_t completed) {
            cancellation_checkpoints.push_back(completed);
        },
        [](long status_code,
           std::span<const onedrive::http::HttpHeader> headers) {
            return status_code == 206 &&
                   std::ranges::any_of(
                       headers,
                       [](const onedrive::http::HttpHeader& header) {
                           return header.name == "Content-Range" &&
                                  header.value == "bytes 6-15/16";
                       }
                   );
        }
    );
    checkpointed_cancel_server.join();
    const auto checkpointed_contents = onedrive::test::read_file(destination);
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (checkpointed_cancel_response ||
        checkpointed_cancel_response.error().code !=
            onedrive::http::HttpErrorCode::cancelled ||
        cancellation_checkpoints != std::vector<std::uint64_t>{11} ||
        checkpointed_contents != "prefixchunk") {
        return fail(
            "cancelled validated HTTP chunk was not checkpointed durably"
        );
    }

    server_error.clear();
    std::jthread stalled_download_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for stalled download request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        std::string request;
        char buffer[4096];
        while (!request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read stalled download request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response_start{"HTTP/1.1 200 OK\r\n"
                                                  "Content-Length: 10\r\n"
                                                  "Connection: close\r\n"
                                                  "\r\n"
                                                  "x"};
        if (::send(
                connection.get(),
                response_start.data(),
                response_start.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response_start.size())) {
            server_error = "cannot start stalled download response";
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds{2});
    }};
    const auto stalled_download = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/stall",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .low_speed_timeout = std::chrono::seconds{1},
            .low_speed_limit_bytes_per_second = 100,
        },
        destination
    );
    stalled_download_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (stalled_download || std::filesystem::exists(destination)) {
        std::filesystem::remove(destination, ignored);
        return fail("stalled HTTP download was not aborted and cleaned up");
    }

    server_error.clear();
    std::jthread reset_server{[&] {
        for (std::size_t index = 0; index < 2; ++index) {
            pollfd descriptor{
                .fd = listener.get(),
                .events = POLLIN,
                .revents = 0,
            };
            if (::poll(&descriptor, 1, 10'000) != 1) {
                server_error = "timed out waiting for reset HTTP connection";
                return;
            }
            Socket connection{
                ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
            };
            if (connection.get() == -1) {
                server_error = "cannot accept reset HTTP connection";
                return;
            }
            const linger reset{
                .l_onoff = 1,
                .l_linger = 0,
            };
            if (::setsockopt(
                    connection.get(),
                    SOL_SOCKET,
                    SO_LINGER,
                    &reset,
                    sizeof(reset)
                ) == -1) {
                server_error = "cannot configure reset HTTP connection";
                return;
            }
        }
    }};
    std::array<bool, 2> reset_requests_failed{};
    std::array<std::jthread, 2> reset_clients{
        std::jthread{[&, port] {
            onedrive::http::CurlHttpClient reset_client;
            reset_requests_failed[0] = !reset_client.perform({
                .method = onedrive::http::HttpMethod::post,
                .url =
                    "http://127.0.0.1:" + std::to_string(port) + "/reset-one",
                .body = std::string(256U * 1024U, 'x'),
                .connect_timeout = std::chrono::seconds{2},
                .operation_timeout = std::chrono::seconds{5},
            });
        }},
        std::jthread{[&, port] {
            onedrive::http::CurlHttpClient reset_client;
            reset_requests_failed[1] = !reset_client.perform({
                .method = onedrive::http::HttpMethod::post,
                .url =
                    "http://127.0.0.1:" + std::to_string(port) + "/reset-two",
                .body = std::string(256U * 1024U, 'x'),
                .connect_timeout = std::chrono::seconds{2},
                .operation_timeout = std::chrono::seconds{5},
            });
        }},
    };
    for (auto& reset_client : reset_clients) {
        reset_client.join();
    }
    reset_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!std::ranges::all_of(reset_requests_failed, [](bool failed) {
            return failed;
        })) {
        return fail("reset concurrent HTTP requests did not fail cleanly");
    }
    return EXIT_SUCCESS;
}
