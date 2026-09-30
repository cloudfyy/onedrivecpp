#include "onedrive/http/http_client.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
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

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
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
    const auto response = client.perform({
        .method = onedrive::http::HttpMethod::post,
        .url = "http://127.0.0.1:" + std::to_string(port) + "/token",
        .headers = {"Content-Type: application/x-www-form-urlencoded"},
        .body = "payload=hello",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    server.join();

    if (!server_error.empty()) {
        return fail(server_error);
    }
    if (!response) {
        return fail(response.error().message);
    }
    if (response->status_code != 201 || response->body != "OK") {
        return fail("HTTP response was not captured correctly");
    }
    if (!received_request.starts_with("POST /token HTTP/1.1") ||
        !received_request.contains("Content-Type: application/x-www-form-urlencoded")) {
        return fail("HTTP request method or headers were not sent correctly");
    }

    return EXIT_SUCCESS;
}
