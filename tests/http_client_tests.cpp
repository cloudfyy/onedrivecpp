#include "onedrive/http/http_client.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>
#include <poll.h>
#include <sstream>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

class Socket {
public:
    explicit Socket(int descriptor) : descriptor_{descriptor} {}

    ~Socket() {
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

class ScopedUmask {
public:
    explicit ScopedUmask(mode_t value) : previous_{::umask(value)} {}

    ~ScopedUmask() {
        ::umask(previous_);
    }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

private:
    mode_t previous_;
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    const ScopedUmask download_umask{0022};
    Socket listener{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (listener.get() == -1) {
        return fail("cannot create HTTP test socket");
    }

    const int reuse_address = 1;
    if (::setsockopt(
            listener.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) == -1) {
        return fail("cannot configure HTTP test socket");
    }

    sockaddr_in address{
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
        .sin_zero = {},
    };
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == -1 ||
        ::listen(listener.get(), 1) == -1) {
        return fail("cannot listen on HTTP test socket");
    }

    socklen_t address_length = sizeof(address);
    if (::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&address),
            &address_length
        ) == -1) {
        return fail("cannot determine HTTP test port");
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

        Socket client{::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)};
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

        constexpr std::string_view response{
            "HTTP/1.1 201 Created\r\n"
            "Content-Type: text/plain\r\n"
            "Retry-After: 17\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n"
            "OK"
        };
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

    const auto port = ntohs(address.sin_port);
    onedrive::http::CurlHttpClient client;
    const auto invalid_transport_options = client.perform({
        .url = "http://127.0.0.1:" + std::to_string(port) + "/invalid",
        .connect_timeout = std::chrono::seconds::zero(),
    });
    if (invalid_transport_options ||
        !invalid_transport_options.error().message.contains(
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
        !received_request.contains("Content-Type: application/x-www-form-urlencoded")) {
        return fail("HTTP request method or headers were not sent correctly");
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
        constexpr std::string_view response{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n"
        };
        static_cast<void>(::send(
            connection.get(),
            response.data(),
            response.size(),
            MSG_NOSIGNAL
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
        constexpr std::string_view download_response{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "download"
        };
        if (::send(
                connection.get(),
                download_response.data(),
                download_response.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(download_response.size())) {
            server_error = "cannot send download response";
        }
    }};
    const auto destination =
        std::filesystem::temp_directory_path() / "onedrive-cpp-http-download";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> download_progress;
    std::string streamed_download_data;
    std::vector<std::uint64_t> streamed_download_offsets;
    const auto download_response = client.download(
        {
            .url = "http://127.0.0.1:" + std::to_string(port) + "/download",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
            .http_version = onedrive::http::HttpVersion::http_2,
        },
        destination,
        [&](std::uint64_t downloaded, std::uint64_t total) {
            download_progress.emplace_back(downloaded, total);
        },
        [&](std::uint64_t offset, std::span<const std::byte> data) {
            streamed_download_offsets.push_back(offset);
            streamed_download_data.append(
                reinterpret_cast<const char*>(data.data()),
                data.size()
            );
        }
    );
    download_server.join();
    std::ifstream downloaded{destination, std::ios::binary};
    const std::string downloaded_contents{
        std::istreambuf_iterator<char>{downloaded},
        std::istreambuf_iterator<char>{}
    };
    struct stat downloaded_status {};
    const bool inspected_download = ::stat(
        destination.c_str(),
        &downloaded_status
    ) == 0;
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
        streamed_download_data != "download" ||
        !inspected_download ||
        (downloaded_status.st_mode & 0777) != 0600) {
        return fail("HTTP response was not streamed to the download file");
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
        constexpr std::string_view response{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "download"
        };
        if (::send(
                connection.get(),
                response.data(),
                response.size(),
                MSG_NOSIGNAL
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
                connection.get(),
                response.data(),
                response.size(),
                MSG_NOSIGNAL
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
        [&](long status_code,
            std::span<const onedrive::http::HttpHeader>) {
            failed_response_inspected = true;
            return status_code == 206;
        }
    );
    failed_chunk_server.join();
    std::ifstream partial{destination, std::ios::binary};
    const std::string partial_contents{
        std::istreambuf_iterator<char>{partial},
        std::istreambuf_iterator<char>{}
    };
    std::filesystem::remove(destination, ignored);
    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!failed_chunk_response ||
        failed_chunk_response->status_code != 503 ||
        partial_contents != "prefix" || !failed_response_inspected ||
        !failed_response_checkpoints.empty()) {
        return fail(
            "rejected HTTP chunk wrote or checkpointed response data"
        );
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
        const std::string header{
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Range: bytes 6-32773/32774\r\n"
            "Content-Length: 32768\r\n"
            "Connection: close\r\n"
            "\r\n"
        };
        const std::string body(20'000, 'x');
        if (::send(
                connection.get(),
                header.data(),
                header.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(header.size()) ||
            ::send(
                connection.get(),
                body.data(),
                body.size(),
                MSG_NOSIGNAL
            ) != static_cast<ssize_t>(body.size())) {
            server_error = "cannot send interrupted chunk response";
        }
    }};
    std::vector<std::uint64_t> durable_checkpoints;
    const auto interrupted_chunk_response = client.download(
        {
            .url =
                "http://127.0.0.1:" + std::to_string(port) + "/interrupted",
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
                                  header.value ==
                                      "bytes 6-32773/32774";
                       }
                   );
        }
    );
    interrupted_chunk_server.join();
    const auto interrupted_size =
        std::filesystem::file_size(destination);
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
            connection.get(),
            remainder.data(),
            remainder.size(),
            MSG_NOSIGNAL
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
    std::ifstream cancelled_partial{destination, std::ios::binary};
    const std::string cancelled_contents{
        std::istreambuf_iterator<char>{cancelled_partial},
        std::istreambuf_iterator<char>{}
    };
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
                server_error =
                    "cannot read checkpointed cancellation request";
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
            server_error =
                "cannot start checkpointed cancellation response";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        constexpr std::string_view remainder{"-tail"};
        static_cast<void>(::send(
            connection.get(),
            remainder.data(),
            remainder.size(),
            MSG_NOSIGNAL
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
    std::ifstream checkpointed_partial{destination, std::ios::binary};
    const std::string checkpointed_contents{
        std::istreambuf_iterator<char>{checkpointed_partial},
        std::istreambuf_iterator<char>{}
    };
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
        constexpr std::string_view response_start{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 10\r\n"
            "Connection: close\r\n"
            "\r\n"
            "x"
        };
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

    std::array<std::string, 2> reused_connection_requests;
    bool reused_connection = false;
    std::ostringstream diagnostic_output;
    auto diagnostic_sink =
        std::make_shared<spdlog::sinks::ostream_sink_mt>(
            diagnostic_output
        );
    auto diagnostic_logger = std::make_shared<spdlog::logger>(
        "http-client-tests",
        diagnostic_sink
    );
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
        const auto receive_request =
            [&](std::string& request, std::string_view body) {
                char buffer[4096];
                while (!request.contains("\r\n\r\n") ||
                       (!body.empty() && !request.contains(body))) {
                    const auto count = ::recv(
                        connection.get(),
                        buffer,
                        sizeof(buffer),
                        0
                    );
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
        constexpr std::string_view first_response{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 1\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "A"
        };
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
        constexpr std::string_view second_response{
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 1\r\n"
            "Connection: close\r\n"
            "\r\n"
            "B"
        };
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
