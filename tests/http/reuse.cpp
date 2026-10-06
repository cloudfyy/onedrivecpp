#include "support.hpp"
#include "onedrive/http/http_client.hpp"
#include "network_test_support.hpp"
#include "test_support.hpp"

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
    std::array<std::string, 2> reused_connection_requests;
    bool reused_connection = false;
    std::ostringstream diagnostic_output;
    auto diagnostic_sink =
        std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostic_output);
    auto diagnostic_logger =
        std::make_shared<spdlog::logger>("http-client-tests", diagnostic_sink);
    diagnostic_logger->set_level(spdlog::level::trace);
    diagnostic_logger->set_pattern("%v");
    spdlog::set_default_logger(diagnostic_logger);
    server_error.clear();
    std::jthread connection_reuse_server{[&] {
        pollfd listener_descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&listener_descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for reusable HTTP connection";
            return;
        }
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        if (connection.get() == -1) {
            server_error = "cannot accept reusable HTTP connection";
            return;
        }
        const auto receive_request = [&](std::string& request,
                                         std::string_view body) {
            char buffer[4096];
            while (!request.contains("\r\n\r\n") ||
                   (!body.empty() && !request.contains(body))) {
                const auto count =
                    ::recv(connection.get(), buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    return false;
                }
                request.append(buffer, static_cast<std::size_t>(count));
            }
            return true;
        };
        const auto send_response = [&](std::string_view response) {
            std::size_t sent = 0;
            while (sent < response.size()) {
                const auto count = ::send(
                    connection.get(),
                    response.data() + sent,
                    response.size() - sent,
                    MSG_NOSIGNAL
                );
                if (count <= 0) {
                    return false;
                }
                sent += static_cast<std::size_t>(count);
            }
            return true;
        };
        if (!receive_request(reused_connection_requests[0], "state=first")) {
            server_error = "cannot read first reusable HTTP request";
            return;
        }
        constexpr std::string_view first_response{"HTTP/1.1 200 OK\r\n"
                                                  "Content-Length: 1\r\n"
                                                  "Connection: keep-alive\r\n"
                                                  "\r\n"
                                                  "A"};
        if (!send_response(first_response)) {
            server_error = "cannot send first reusable HTTP response";
            return;
        }

        pollfd connection_descriptor{
            .fd = connection.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&connection_descriptor, 1, 10'000) != 1 ||
            (connection_descriptor.revents & POLLIN) == 0) {
            server_error = "HTTP connection was not reused";
            return;
        }
        if (!receive_request(reused_connection_requests[1], {})) {
            server_error = "cannot read second request on reused connection";
            return;
        }
        reused_connection = true;
        constexpr std::string_view second_response{"HTTP/1.1 200 OK\r\n"
                                                   "Content-Length: 1\r\n"
                                                   "Connection: close\r\n"
                                                   "\r\n"
                                                   "B"};
        if (!send_response(second_response)) {
            server_error = "cannot send second reusable HTTP response";
        }
    }};
    const auto first_reused_response = client.perform({
        .method = onedrive::http::HttpMethod::post,
        .url = "http://127.0.0.1:" + std::to_string(port) + "/reuse-first",
        .headers = {"X-Reuse-Test: first"},
        .body = "state=first",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
    });
    const auto second_reused_response = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/reuse-second",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
    });
    connection_reuse_server.join();
    diagnostic_logger->flush();
    const auto diagnostic_log = diagnostic_output.str();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!first_reused_response || first_reused_response->body != "A" ||
        !second_reused_response || second_reused_response->body != "B" ||
        !reused_connection ||
        !reused_connection_requests[0].starts_with(
            "POST /reuse-first HTTP/1.1"
        ) ||
        !reused_connection_requests[0].contains("X-Reuse-Test: first") ||
        !reused_connection_requests[0].contains("state=first") ||
        !reused_connection_requests[1].starts_with(
            "GET /reuse-second HTTP/1.1"
        ) ||
        reused_connection_requests[1].contains("X-Reuse-Test: first") ||
        reused_connection_requests[1].contains("state=first") ||
        !diagnostic_log.contains("http_version=1.1") ||
        !diagnostic_log.contains("new_connections=1") ||
        !diagnostic_log.contains("new_connections=0") ||
        !diagnostic_log.contains("dns_us=") ||
        !diagnostic_log.contains("tcp_connect_us=") ||
        !diagnostic_log.contains("tls_handshake_us=") ||
        !diagnostic_log.contains("server_wait_us=") ||
        !diagnostic_log.contains("transfer_us=") ||
        !diagnostic_log.contains("total_us=") ||
        diagnostic_log.contains("/reuse-first") ||
        diagnostic_log.contains("X-Reuse-Test") ||
        diagnostic_log.contains("state=first")) {
        return fail("HTTP handle reuse leaked request state");
    }
    return EXIT_SUCCESS;
}
