#include "support.hpp"
#include "http/callbacks.hpp"
#include "http/curl.hpp"
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
    static_assert(noexcept(onedrive::http::detail::read_request_body(
        nullptr, 0, 0, nullptr
    )));
    static_assert(noexcept(onedrive::http::detail::write_response(
        nullptr, 0, 0, nullptr
    )));
    static_assert(noexcept(onedrive::http::detail::write_header(
        nullptr, 0, 0, nullptr
    )));
    static_assert(noexcept(onedrive::http::detail::report_progress(
        nullptr, 0, 0, 0, 0
    )));

    try {
        static_cast<void>(onedrive::http::detail::curl_proxy_auth(
            static_cast<onedrive::http::ProxyAuth>(-1)
        ));
        return fail("invalid proxy authentication mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    onedrive::http::detail::CallbackFailure callback_failure;
    try {
        throw std::bad_alloc{};
    } catch (...) {
        onedrive::http::detail::record_callback_failure(
            callback_failure, std::current_exception()
        );
    }
    if (callback_failure.kind !=
        onedrive::http::detail::CallbackFailureKind::allocation) {
        return fail("callback allocation failure was not preserved");
    }
    onedrive::http::UploadThrottle failing_throttle{
        [](std::size_t, const std::stop_token&) -> bool {
            throw std::bad_alloc{};
        }
    };
    onedrive::http::detail::ReadContext read_context{
        .body = "body",
        .upload_throttle = &failing_throttle,
    };
    char upload_buffer[4]{};
    if (onedrive::http::detail::read_request_body(
            upload_buffer,
            1,
            sizeof(upload_buffer),
            &read_context
        ) != CURL_READFUNC_ABORT ||
        read_context.failure.kind !=
            onedrive::http::detail::CallbackFailureKind::throttle ||
        !read_context.failure.exception) {
        return fail("upload callback allocation failure crossed the C boundary");
    }
    onedrive::http::DownloadProgress failing_progress{
        [](std::uint64_t, std::uint64_t) {
            throw std::bad_alloc{};
        }
    };
    onedrive::http::detail::ProgressContext progress_context{
        .callback = &failing_progress,
    };
    if (onedrive::http::detail::report_progress(
            &progress_context, 1, 1, 0, 0
        ) == 0 ||
        !progress_context.failed ||
        !progress_context.exception) {
        return fail(
            "progress callback allocation failure crossed the C boundary"
        );
    }
    try {
        throw 42;
    } catch (...) {
        onedrive::http::detail::record_callback_failure(
            callback_failure, std::current_exception()
        );
    }
    if (callback_failure.kind !=
        onedrive::http::detail::CallbackFailureKind::internal) {
        return fail("unknown callback failure was not preserved");
    }
    if (onedrive::http::detail::callback_failure_detail(callback_failure) !=
        "unknown error") {
        return fail("unknown callback exception diagnostic was incorrect");
    }
    callback_failure = {
        .kind = onedrive::http::detail::CallbackFailureKind::throttle,
        .system_error = 0,
        .detail = "literal callback detail",
        .exception = {},
    };
    if (onedrive::http::detail::callback_failure_detail(callback_failure) !=
        "literal callback detail") {
        return fail("literal callback failure detail was not preserved");
    }
    callback_failure = {};
    onedrive::http::detail::record_callback_failure(callback_failure, {});
    if (callback_failure.kind !=
            onedrive::http::detail::CallbackFailureKind::internal ||
        onedrive::http::detail::callback_failure_detail(callback_failure) !=
            "unknown error") {
        return fail("missing callback exception was not classified");
    }
    using onedrive::http::detail::CallbackFailureKind;
    using onedrive::http::detail::CallbackStorage;
    using onedrive::http::detail::callback_storage_error;
    if (callback_storage_error(
            CallbackFailureKind::allocation,
            CallbackStorage::response_headers
        ) != "cannot allocate HTTP response header storage" ||
        callback_storage_error(
            CallbackFailureKind::allocation,
            CallbackStorage::response_body
        ) != "cannot allocate HTTP response body storage" ||
        callback_storage_error(
            CallbackFailureKind::internal,
            CallbackStorage::response_headers
        ) != "HTTP response header callback failed" ||
        callback_storage_error(
            CallbackFailureKind::internal,
            CallbackStorage::response_body
        ) != "HTTP response body callback failed" ||
        !callback_storage_error(
             CallbackFailureKind::none,
             CallbackStorage::response_body
         )
             .empty()) {
        return fail("callback storage failure diagnostics were incorrect");
    }

    const ScopedUmask download_umask{0022};
    const onedrive::test::TemporaryDirectory temporary;
    std::error_code ignored;
    auto http_listener = onedrive::test::create_loopback_listener(4);
    auto& listener = http_listener.socket;
    if (listener.get() == -1) {
        return fail("cannot create HTTP test socket");
    }

    std::string received_request;
    std::string server_error;
    std::jthread server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 10'000) != 1) {
            server_error = "timed out waiting for HTTP request";
            return;
        }

        Socket client{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        if (client.get() == -1) {
            server_error = "cannot accept HTTP test connection";
            return;
        }

        char buffer[4096];
        while (!received_request.contains("payload=hello")) {
            const auto count = ::recv(client.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read complete HTTP test request";
                return;
            }
            received_request.append(buffer, static_cast<std::size_t>(count));
        }

        constexpr std::string_view response{"HTTP/1.1 201 Created\r\n"
                                            "Content-Type: text/plain\r\n"
                                            "Retry-After: 17\r\n"
                                            "Content-Length: 2\r\n"
                                            "Connection: close\r\n"
                                            "\r\n"
                                            "OK"};
        std::size_t sent = 0;
        while (sent < response.size()) {
            const auto count = ::send(
                client.get(),
                response.data() + sent,
                response.size() - sent,
                MSG_NOSIGNAL
            );
            if (count <= 0) {
                server_error = "cannot send HTTP test response";
                return;
            }
            sent += static_cast<std::size_t>(count);
        }
    }};

    const auto port = http_listener.port;
    onedrive::http::CurlHttpClient client;
    const auto invalid_transport_options = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/invalid",
        .connect_timeout = std::chrono::seconds::zero(),
    });
    const auto invalid_send_limit = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/invalid",
        .maximum_send_speed_bytes_per_second =
            std::numeric_limits<std::uint64_t>::max(),
    });
    if (invalid_transport_options || invalid_send_limit ||
        !invalid_transport_options.error().message.contains(
            "invalid transport options"
        ) ||
        !invalid_send_limit.error().message.contains(
            "invalid transport options"
        )) {
        return fail("invalid HTTP transport options were accepted");
    }
    const auto response = client.perform({
        .method = onedrive::http::HttpMethod::post,
        .url = "http://127.0.0.1:" + std::to_string(port) + "/token",
        .headers = {"Content-Type: application/x-www-form-urlencoded"},
        .body = "payload=hello",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
    });
    server.join();

    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!response) {
        return fail(response.error().message);
    }
    if (response->status_code != 201 || response->body != "OK" ||
        response->received_size != 2) {
        return fail("HTTP response was not captured correctly");
    }
    bool retry_after_captured = false;
    for (const auto& header : response->headers) {
        if (header.name == "Retry-After" && header.value == "17") {
            retry_after_captured = true;
        }
    }
    if (!retry_after_captured) {
        return fail("HTTP response headers were not captured correctly");
    }
    if (!received_request.starts_with("POST /token HTTP/1.1") ||
        !received_request.contains(
            "Content-Type: application/x-www-form-urlencoded"
        )) {
        return fail("HTTP request method or headers were not sent correctly");
    }

    std::string put_request;
    server_error.clear();
    std::jthread put_server{[&] {
        Socket client{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        if (client.get() == -1) {
            server_error = "cannot accept HTTP PUT test connection";
            return;
        }
        char buffer[4096];
        while (!put_request.contains("upload-body")) {
            const auto count = ::recv(client.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                server_error = "cannot read complete HTTP PUT request";
                return;
            }
            put_request.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                            "Content-Length: 2\r\n"
                                            "Connection: close\r\n"
                                            "\r\n"
                                            "OK"};
        static_cast<void>(
            ::send(client.get(), response.data(), response.size(), MSG_NOSIGNAL)
        );
    }};
    std::size_t throttled_upload_bytes = 0;
    const auto put_response = client.perform({
        .method = onedrive::http::HttpMethod::put,
        .url = "http://127.0.0.1:" + std::to_string(port) + "/content",
        .headers = {"Content-Type: application/octet-stream"},
        .body = "upload-body",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
        .upload_throttle = [&](std::size_t bytes, const std::stop_token&) {
            throttled_upload_bytes += bytes;
            return true;
        },
    });
    put_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!put_response || put_response->status_code != 200 ||
        !put_request.starts_with("PUT /content HTTP/1.1") ||
        !put_request.ends_with("upload-body") ||
        throttled_upload_bytes != std::string_view{"upload-body"}.size()) {
        return fail("HTTP PUT request body was not sent correctly");
    }

    std::string unthrottled_put_request;
    server_error.clear();
    std::jthread upload_throttle_error_server{[&] {
        for (std::size_t request_index = 0; request_index < 3;
             ++request_index) {
            Socket connection{
                ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
            };
            if (connection.get() == -1) {
                server_error = "cannot accept upload throttle test connection";
                return;
            }
            char buffer[4096];
            std::string request;
            while (true) {
                const auto count =
                    ::recv(connection.get(), buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    break;
                }
                request.append(buffer, static_cast<std::size_t>(count));
                if (request_index == 2 && request.contains("plain-upload")) {
                    break;
                }
            }
            if (request_index != 2) {
                continue;
            }
            unthrottled_put_request = std::move(request);
            constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                                "Content-Length: 2\r\n"
                                                "Connection: close\r\n"
                                                "\r\n"
                                                "OK"};
            if (::send(
                    connection.get(),
                    response.data(),
                    response.size(),
                    MSG_NOSIGNAL
                ) != static_cast<ssize_t>(response.size())) {
                server_error = "cannot send unthrottled upload response";
                return;
            }
        }
    }};
    const auto cancelled_upload_response = client.perform({
        .method = onedrive::http::HttpMethod::put,
        .url = "http://127.0.0.1:" + std::to_string(port) +
               "/upload-throttle-cancel",
        .body = "cancelled-upload",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
        .upload_throttle = [](std::size_t, const std::stop_token&) {
            return false;
        },
    });
    const auto failed_upload_response = client.perform({
        .method = onedrive::http::HttpMethod::put,
        .url = "http://127.0.0.1:" + std::to_string(port) +
               "/upload-throttle-failure",
        .body = "failed-upload",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
        .upload_throttle = [](std::size_t, const std::stop_token&) -> bool {
            throw std::runtime_error{"simulated upload throttle failure"};
        },
    });
    const auto unthrottled_put_response = client.perform({
        .method = onedrive::http::HttpMethod::put,
        .url =
            "http://127.0.0.1:" + std::to_string(port) + "/unthrottled-content",
        .body = "plain-upload",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
        .http_version = onedrive::http::HttpVersion::http_1_1,
    });
    upload_throttle_error_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (cancelled_upload_response ||
        cancelled_upload_response.error().code !=
            onedrive::http::HttpErrorCode::cancelled) {
        return fail("upload throttle cancellation was not propagated");
    }
    if (failed_upload_response ||
        !failed_upload_response.error().message.contains(
            "simulated upload throttle failure"
        )) {
        return fail("upload throttle failure was not propagated");
    }
    if (!unthrottled_put_response ||
        unthrottled_put_response->status_code != 200 ||
        !unthrottled_put_request.starts_with(
            "PUT /unthrottled-content HTTP/1.1"
        ) ||
        !unthrottled_put_request.ends_with("plain-upload")) {
        return fail("upload throttle state leaked to the next request");
    }

    bool unexpected_ipv4_connection = false;
    std::jthread forced_ipv6_server{[&] {
        pollfd descriptor{
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        const auto poll_result = ::poll(&descriptor, 1, 1'000);
        if (poll_result == -1) {
            server_error = "cannot poll for forced IPv6 request";
            return;
        }
        if (poll_result == 0) {
            return;
        }
        unexpected_ipv4_connection = true;
        Socket connection{
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
        constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                            "Content-Length: 0\r\n"
                                            "Connection: close\r\n"
                                            "\r\n"};
        static_cast<void>(::send(
            connection.get(), response.data(), response.size(), MSG_NOSIGNAL
        ));
    }};
    const auto forced_ipv6_response = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/ipv6-only",
        .connect_timeout = std::chrono::seconds{1},
        .operation_timeout = std::chrono::seconds{2},
        .ip_version = onedrive::http::IpVersion::ipv6,
    });
    forced_ipv6_server.join();
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (forced_ipv6_response || unexpected_ipv4_connection) {
        return fail("forced IPv6 request used an IPv4 destination");
    }

    const auto missing_redirect_limit = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) +
               "/missing-redirect-limit",
        .connect_timeout = std::chrono::seconds{1},
        .operation_timeout = std::chrono::seconds{2},
        .follow_redirects = true,
    });
    const auto unused_redirect_limit = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) +
               "/unused-redirect-limit",
        .connect_timeout = std::chrono::seconds{1},
        .operation_timeout = std::chrono::seconds{2},
        .maximum_redirects = 5,
    });
    if (missing_redirect_limit || unused_redirect_limit ||
        !missing_redirect_limit.error().message.contains(
            "invalid transport options"
        ) ||
        !unused_redirect_limit.error().message.contains(
            "invalid transport options"
        )) {
        return fail("invalid HTTP redirect options were accepted");
    }
    return EXIT_SUCCESS;
}
