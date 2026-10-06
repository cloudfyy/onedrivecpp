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
    std::string download_request;
    server_error.clear();
    std::jthread download_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for download request";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        char buffer[4096];
        while (!download_request.contains("\r\n\r\n")) {
            const auto count =
                ::recv(connection.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read download request";
                return;
            }
            download_request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view download_response{"HTTP/1.1 200 OK\r\n"
                                                     "Content-Length: 8\r\n"
                                                     "Connection: close\r\n"
                                                     "\r\n"
                                                     "download"};
        if (::send(
                connection.get(),
                download_response.data(),
                download_response.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(download_response.size())) {
            server_error = "cannot send download response";
        }
    }};
    const auto destination = temporary.path() / "download";
    std::vector<std::pair<std::uint64_t, std::uint64_t>> download_progress;
    std::string streamed_download_data;
    std::vector<std::uint64_t> streamed_download_offsets;
    std::size_t throttled_download_bytes = 0;
    const auto download_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/download",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .http_version = onedrive::http::HttpVersion::http_2,
            .download_throttle =
                [&](std::size_t bytes, const std::stop_token&) {
                    throttled_download_bytes += bytes;
                    return true;
                },
        },
        destination,
        [&](std::uint64_t downloaded, std::uint64_t total) {
            download_progress.emplace_back(downloaded, total);
        },
        [&](std::uint64_t offset, std::span<const std::byte> data) {
            streamed_download_offsets.push_back(offset);
            streamed_download_data.append(
                reinterpret_cast<const char*>(data.data()), data.size()
            );
        }
    );
    download_server.join();
    const auto downloaded_contents = onedrive::test::read_file(destination);
    struct stat downloaded_status{};
    const bool inspected_download =
        ::stat(destination.c_str(), &downloaded_status) == 0;
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!download_response || download_response->status_code != 200 ||
        download_response->received_size != 8 ||
        downloaded_contents != "download" ||
        !download_request.starts_with("GET /download HTTP/1.1") ||
        download_progress.empty() || download_progress.back().first != 8 ||
        download_progress.back().second != 8 ||
        streamed_download_offsets != std::vector<std::uint64_t>{0} ||
        streamed_download_data != "download" || throttled_download_bytes != 8 ||
        !inspected_download || (downloaded_status.st_mode & 0777) != 0600) {
        return fail("HTTP response was not streamed to the download file");
    }

    server_error.clear();
    std::jthread cancelled_throttle_server{[&] {
        for (std::size_t index = 0; index < 2; ++index) {
            pollfd descriptor{
                .fd = listener.get(),
                .events = POLLIN,
                .revents = 0,
            };
            if (::poll(&descriptor, 1, 10'000) != 1) {
                server_error = "timed out waiting for throttled request";
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
                    server_error = "cannot read throttled request";
                    return;
                }
                request.append(buffer, static_cast<std::size_t>(count));
            }
            constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                                "Content-Length: 8\r\n"
                                                "Connection: close\r\n"
                                                "\r\n"
                                                "download"};
            if (::send(
                    connection.get(),
                    response.data(),
                    response.size(),
                    MSG_NOSIGNAL
                ) != static_cast<ssize_t>(response.size())) {
                server_error = "cannot send throttled response";
                return;
            }
        }
    }};
    const auto cancelled_throttle_response = client.download(
        {
            .url =
                "http://127.0.0.1:" + std::to_string(port) + "/throttle-cancel",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_throttle = [](std::size_t,
                                    const std::stop_token&) { return false; },
        },
        destination
    );
    const auto failed_throttle_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) +
                   "/throttle-failure",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .download_throttle = [](std::size_t,
                                    const std::stop_token&) -> bool {
                throw std::runtime_error{"simulated throttle failure"};
            },
        },
        destination
    );
    cancelled_throttle_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (cancelled_throttle_response ||
        cancelled_throttle_response.error().code !=
            onedrive::http::HttpErrorCode::cancelled ||
        std::filesystem::exists(destination)) {
        return fail("download throttle cancellation was not propagated");
    }
    if (failed_throttle_response ||
        !failed_throttle_response.error().message.contains(
            "simulated throttle failure"
        ) ||
        std::filesystem::exists(destination)) {
        return fail("download throttle failure was not propagated");
    }

    server_error.clear();
    std::jthread failed_observer_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for observed download request";
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
                server_error = "cannot read observed download request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                            "Content-Length: 8\r\n"
                                            "Connection: close\r\n"
                                            "\r\n"
                                            "download"};
        if (::send(
                connection.get(), response.data(), response.size(), MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response.size())) {
            server_error = "cannot send observed download response";
        }
    }};
    const auto failed_observer_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/observer",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
        },
        destination,
        {},
        [](std::uint64_t, std::span<const std::byte>) {
            throw std::runtime_error{"simulated observer failure"};
        }
    );
    failed_observer_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (failed_observer_response ||
        !failed_observer_response.error().message.contains(
            "simulated observer failure"
        ) ||
        std::filesystem::exists(destination)) {
        return fail("failed download data observer did not abort cleanly");
    }
    return EXIT_SUCCESS;
}
