#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace onedrive::test {

using Socket = onedrive::util::UniqueFD;

struct TcpListener {
    Socket socket;
    std::uint16_t port{};
};

[[nodiscard]] inline TcpListener create_loopback_listener(int backlog = 1) {
    Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (socket.get() == -1) {
        return {};
    }
    const int reuse_address = 1;
    if (::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) == -1) {
        return {};
    }
    sockaddr_in address{
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
        .sin_zero = {},
    };
    if (::bind(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == -1 ||
        ::listen(socket.get(), backlog) == -1) {
        return {};
    }
    socklen_t length = sizeof(address);
    if (::getsockname(
            socket.get(),
            reinterpret_cast<sockaddr*>(&address),
            &length
        ) == -1) {
        return {};
    }
    return {
        .socket = std::move(socket),
        .port = ntohs(address.sin_port),
    };
}

[[nodiscard]] inline bool wait_for_connection(
    int listener,
    std::chrono::milliseconds timeout = std::chrono::seconds{10}
) {
    pollfd descriptor{
        .fd = listener,
        .events = POLLIN,
        .revents = 0,
    };
    return ::poll(
               &descriptor,
               1,
               static_cast<int>(timeout.count())
           ) == 1;
}

[[nodiscard]] inline bool receive_exact(
    int socket,
    std::span<std::byte> data
) {
    std::size_t received = 0;
    while (received < data.size()) {
        const auto count = ::recv(
            socket,
            data.data() + received,
            data.size() - received,
            0
        );
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] inline bool send_all(
    int socket,
    std::span<const std::byte> data
) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto count = ::send(
            socket,
            data.data() + sent,
            data.size() - sent,
            MSG_NOSIGNAL
        );
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] bool send_all(
    int socket,
    const std::array<std::byte, Size>& data
) {
    return send_all(socket, std::span<const std::byte>{data});
}

[[nodiscard]] inline bool receive_http_headers(
    int socket,
    std::string& headers,
    std::size_t maximum_size = std::size_t{64} * 1024U
) {
    std::array<char, 1024> buffer{};
    while (!headers.contains("\r\n\r\n")) {
        if (headers.size() >= maximum_size) {
            return false;
        }
        const auto count =
            ::recv(socket, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            return false;
        }
        headers.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return headers.size() <= maximum_size;
}

}  // namespace onedrive::test
