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
    auto http_listener = onedrive::test::create_loopback_listener(4);
    auto& listener = http_listener.socket;
    if (listener.get() == -1) {
        return fail("cannot create HTTP test socket");
    }
    const auto port = http_listener.port;
    onedrive::http::CurlHttpClient client;
    std::string server_error;
    server_error.clear();
    std::jthread multiple_response_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for multiple-response request";
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
                server_error = "cannot read multiple-response request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response{"HTTP/1.1 103 Early Hints\r\n"
                                            "X-Intermediate: discard\r\n"
                                            "\r\n"
                                            "HTTP/1.1 200 OK\r\n"
                                            "X-Final: retained\r\n"
                                            "Content-Length: 2\r\n"
                                            "Connection: close\r\n"
                                            "\r\n"
                                            "OK"};
        if (::send(
                connection.get(), response.data(), response.size(), MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response.size())) {
            server_error = "cannot send multiple HTTP responses";
        }
    }};
    const auto multiple_response = client.perform({
        .url =
            "http://127.0.0.1:" + std::to_string(port) + "/multiple-responses",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    multiple_response_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!multiple_response || multiple_response->body != "OK" ||
        std::ranges::any_of(
            multiple_response->headers,
            [](const onedrive::http::HttpHeader& header) {
                return header.name == "X-Intermediate";
            }
        ) ||
        !std::ranges::any_of(
            multiple_response->headers,
            [](const onedrive::http::HttpHeader& header) {
                return header.name == "X-Final" && header.value == "retained";
            }
        )) {
        return fail("HTTP response headers were mixed across responses");
    }

    server_error.clear();
    std::jthread unsafe_redirect_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for redirect request";
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
                server_error = "cannot read redirect request";
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        const std::string response{
            "HTTP/1.1 302 Found\r\n"
            "Location: http://127.0.0.1:" +
            std::to_string(port) +
            "/unsafe-target\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n"
        };
        if (::send(
                connection.get(), response.data(), response.size(), MSG_NOSIGNAL
            ) != static_cast<ssize_t>(response.size())) {
            server_error = "cannot send redirect response";
        }
    }};
    const auto unsafe_redirect = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/redirect-source",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .follow_redirects = true,
        .maximum_redirects = 5,
    });
    unsafe_redirect_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (unsafe_redirect || unsafe_redirect.error().code !=
                               onedrive::http::HttpErrorCode::redirect) {
        return fail("HTTP redirect followed a non-HTTPS target");
    }
    return EXIT_SUCCESS;
}
