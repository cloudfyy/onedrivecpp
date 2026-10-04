#include "onedrive/http/http_client.hpp"
#include "test_support.hpp"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

class Socket {
public:
    Socket() = default;

    explicit Socket(int descriptor) : descriptor_{descriptor} {}

    ~Socket() {
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : descriptor_{std::exchange(other.descriptor_, -1)} {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (descriptor_ != -1) {
                ::close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

struct Listener {
    Socket socket;
    std::uint16_t port{};
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

Listener create_listener() {
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
        ::listen(socket.get(), 1) == -1) {
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

bool wait_for_connection(int listener) {
    pollfd descriptor{
        .fd = listener,
        .events = POLLIN,
        .revents = 0,
    };
    return ::poll(&descriptor, 1, 10'000) == 1;
}

bool receive_exact(int socket, std::span<std::byte> data) {
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

bool send_all(int socket, std::span<const std::byte> data) {
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

bool receive_http_headers(int socket, std::string& headers) {
    std::array<char, 1024> buffer{};
    while (!headers.contains("\r\n\r\n")) {
        const auto count =
            ::recv(socket, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            return false;
        }
        headers.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return true;
}

bool send_http_ok(int socket) {
    constexpr std::string_view response{
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK"
    };
    return send_all(socket, std::as_bytes(std::span{response}));
}

void write_file(
    const std::filesystem::path& path,
    std::string_view contents
) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size())
    );
}

bool proxy_password_rejected(
    const std::filesystem::path& path,
    std::string_view expected_error
) {
    try {
        const onedrive::http::CurlHttpClient client{{
            .url = "socks5h://127.0.0.1:1",
            .username = "proxy-user",
            .password_file = path,
        }};
        return false;
    } catch (const std::runtime_error& error) {
        return std::string_view{error.what()}.contains(expected_error);
    }
}

template <std::size_t Size>
bool send_all(int socket, const std::array<std::byte, Size>& data) {
    return send_all(socket, std::span<const std::byte>{data});
}

}  // namespace

int main() {
    const onedrive::test::TemporaryDirectory temporary;
    auto socks_listener = create_listener();
    if (socks_listener.socket.get() == -1) {
        return fail("cannot create SOCKS5 test listener");
    }

    const auto password_path = temporary.path() / "proxy-password";
    if (!proxy_password_rejected(password_path, "cannot safely open")) {
        return fail("missing proxy password file was accepted");
    }
    const auto directory_path = temporary.path() / "password-directory";
    std::filesystem::create_directory(directory_path);
    if (!proxy_password_rejected(directory_path, "not a regular file")) {
        return fail("proxy password directory was accepted");
    }

    write_file(password_path, "");
    if (::chmod(password_path.c_str(), S_IRUSR | S_IWUSR) == -1 ||
        !proxy_password_rejected(password_path, "must not be empty")) {
        return fail("empty proxy password file was accepted");
    }
    const std::string nul_password{"secret\0suffix", 13};
    write_file(password_path, nul_password);
    if (!proxy_password_rejected(password_path, "NUL bytes")) {
        return fail("proxy password containing NUL was accepted");
    }
    write_file(
        password_path,
        std::string(std::size_t{64} * 1024U + 1U, 'x')
    );
    if (!proxy_password_rejected(password_path, "64 KiB")) {
        return fail("oversized proxy password file was accepted");
    }

    write_file(password_path, "proxy-secret\r\n");
    if (::chmod(
            password_path.c_str(),
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
        ) == -1) {
        return fail("cannot set insecure proxy password test permissions");
    }
    if (!proxy_password_rejected(
            password_path,
            "group or other permissions"
        )) {
        return fail("insecure proxy password file was accepted");
    }
    if (::chmod(password_path.c_str(), S_IRUSR | S_IWUSR) == -1) {
        return fail("cannot secure proxy password test file");
    }
    const auto symlink_path = temporary.path() / "password-link";
    std::filesystem::create_symlink(password_path, symlink_path);
    if (!proxy_password_rejected(symlink_path, "cannot safely open")) {
        return fail("proxy password symbolic link was accepted");
    }

    std::string socks_error;
    std::string proxied_host;
    std::string proxy_username;
    std::string proxy_password;
    std::jthread socks_server{[&] {
        if (!wait_for_connection(socks_listener.socket.get())) {
            socks_error = "timed out waiting for SOCKS5 connection";
            return;
        }
        Socket connection{
            ::accept4(
                socks_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
        std::array<std::byte, 2> greeting_header{};
        if (connection.get() == -1 ||
            !receive_exact(connection.get(), greeting_header) ||
            greeting_header[0] != std::byte{5}) {
            socks_error = "invalid SOCKS5 greeting";
            return;
        }
        const auto method_count =
            std::to_integer<std::size_t>(greeting_header[1]);
        std::string methods(method_count, '\0');
        if (method_count == 0 ||
            !receive_exact(
                connection.get(),
                std::as_writable_bytes(std::span{methods})
            ) ||
            !methods.contains('\2')) {
            socks_error =
                "SOCKS5 proxy did not offer username/password mode";
            return;
        }
        const std::array method_reply{std::byte{5}, std::byte{2}};
        if (!send_all(connection.get(), method_reply)) {
            socks_error = "cannot send SOCKS5 method reply";
            return;
        }

        std::array<std::byte, 2> auth_header{};
        if (!receive_exact(connection.get(), auth_header) ||
            auth_header[0] != std::byte{1}) {
            socks_error = "invalid SOCKS5 username/password request";
            return;
        }
        proxy_username.assign(
            std::to_integer<std::size_t>(auth_header[1]),
            '\0'
        );
        std::array<std::byte, 1> password_length{};
        if (!receive_exact(
                connection.get(),
                std::as_writable_bytes(std::span{proxy_username})
            ) ||
            !receive_exact(connection.get(), password_length)) {
            socks_error = "incomplete SOCKS5 username";
            return;
        }
        proxy_password.assign(
            std::to_integer<std::size_t>(password_length[0]),
            '\0'
        );
        if (!receive_exact(
                connection.get(),
                std::as_writable_bytes(std::span{proxy_password})
            )) {
            socks_error = "incomplete SOCKS5 password";
            return;
        }
        const std::array auth_reply{std::byte{1}, std::byte{0}};
        if (!send_all(connection.get(), auth_reply)) {
            socks_error = "cannot send SOCKS5 authentication reply";
            return;
        }

        std::array<std::byte, 5> request{};
        if (!receive_exact(connection.get(), request) ||
            request[0] != std::byte{5} ||
            request[1] != std::byte{1} ||
            request[3] != std::byte{3}) {
            socks_error = "invalid SOCKS5 domain request";
            return;
        }
        const auto host_size = std::to_integer<std::size_t>(request[4]);
        std::string host(host_size, '\0');
        std::array<std::byte, 2> target_port{};
        if (!receive_exact(
                connection.get(),
                std::as_writable_bytes(std::span{host})
            ) ||
            !receive_exact(connection.get(), target_port)) {
            socks_error = "incomplete SOCKS5 domain request";
            return;
        }
        proxied_host = host;

        const std::array connect_reply{
            std::byte{5},
            std::byte{0},
            std::byte{0},
            std::byte{1},
            std::byte{127},
            std::byte{0},
            std::byte{0},
            std::byte{1},
            std::byte{0},
            std::byte{80},
        };
        if (!send_all(connection.get(), connect_reply)) {
            socks_error = "cannot send SOCKS5 connect reply";
            return;
        }

        std::string request_headers;
        if (!receive_http_headers(connection.get(), request_headers)) {
            socks_error = "cannot read proxied HTTP request";
            return;
        }
        if (!send_http_ok(connection.get())) {
            socks_error = "cannot send proxied HTTP response";
        }
    }};

    ::setenv("NO_PROXY", "*", 1);
    ::setenv("no_proxy", "*", 1);
    onedrive::http::CurlHttpClient socks_client{{
        .url = "socks5h://127.0.0.1:" +
               std::to_string(socks_listener.port),
        .no_proxy = std::vector<std::string>{},
        .username = "proxy-user",
        .password_file = password_path,
    }};
    const auto socks_response = socks_client.perform({
        .url = "http://proxy-target.example:8080/resource",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    socks_server.join();
    if (!socks_error.empty()) {
        return fail(socks_error);
    }
    if (!socks_response || socks_response->status_code != 200 ||
        socks_response->body != "OK") {
        return fail(
            socks_response ?
                "SOCKS5 proxy returned an unexpected response" :
                socks_response.error().message
        );
    }
    if (proxied_host != "proxy-target.example") {
        return fail("SOCKS5H did not delegate DNS resolution to the proxy");
    }
    if (proxy_username != "proxy-user" ||
        proxy_password != "proxy-secret") {
        return fail("SOCKS5 proxy credentials were not applied correctly");
    }

    auto basic_proxy_listener = create_listener();
    if (basic_proxy_listener.socket.get() == -1) {
        return fail("cannot create HTTP Basic proxy test listener");
    }
    std::string basic_proxy_error;
    std::string basic_proxy_request;
    std::jthread basic_proxy_server{[&] {
        if (!wait_for_connection(basic_proxy_listener.socket.get())) {
            basic_proxy_error =
                "timed out waiting for HTTP Basic proxy connection";
            return;
        }
        Socket connection{
            ::accept4(
                basic_proxy_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
        if (connection.get() == -1 ||
            !receive_http_headers(
                connection.get(),
                basic_proxy_request
            )) {
            basic_proxy_error =
                "cannot read HTTP Basic proxy request";
            return;
        }
        if (!send_http_ok(connection.get())) {
            basic_proxy_error =
                "cannot send HTTP Basic proxy response";
        }
    }};
    onedrive::http::CurlHttpClient basic_proxy_client{{
        .url = "http://127.0.0.1:" +
               std::to_string(basic_proxy_listener.port),
        .no_proxy = std::vector<std::string>{},
        .username = "proxy-user",
        .password_file = password_path,
        .auth = onedrive::http::ProxyAuth::basic,
    }};
    const auto basic_proxy_response = basic_proxy_client.perform({
        .url = "http://basic-target.example/resource",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    basic_proxy_server.join();
    if (!basic_proxy_error.empty()) {
        return fail(basic_proxy_error);
    }
    if (!basic_proxy_response ||
        basic_proxy_response->status_code != 200 ||
        !basic_proxy_request.starts_with(
            "GET http://basic-target.example/resource HTTP/1.1"
        ) ||
        !basic_proxy_request.contains(
            "Proxy-Authorization: Basic "
            "cHJveHktdXNlcjpwcm94eS1zZWNyZXQ="
        )) {
        return fail("HTTP Basic proxy authentication was not applied");
    }

    ::unsetenv("NO_PROXY");
    ::unsetenv("no_proxy");

    auto bypass_listener = create_listener();
    if (bypass_listener.socket.get() == -1) {
        return fail("cannot create no-proxy test listener");
    }
    std::string bypass_error;
    std::jthread bypass_server{[&] {
        if (!wait_for_connection(bypass_listener.socket.get())) {
            bypass_error = "timed out waiting for direct bypass connection";
            return;
        }
        Socket connection{
            ::accept4(
                bypass_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
        std::string request;
        if (connection.get() == -1 ||
            !receive_http_headers(connection.get(), request) ||
            !send_http_ok(connection.get())) {
            bypass_error = "cannot serve direct bypass request";
        }
    }};
    onedrive::http::CurlHttpClient bypass_client{{
        .url = "http://127.0.0.1:1",
        .no_proxy = std::vector<std::string>{"127.0.0.1"},
    }};
    const auto bypass_response = bypass_client.perform({
        .url = "http://127.0.0.1:" +
               std::to_string(bypass_listener.port) +
               "/direct",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    bypass_server.join();
    if (!bypass_error.empty()) {
        return fail(bypass_error);
    }
    if (!bypass_response || bypass_response->status_code != 200) {
        return fail("configured no-proxy destination used the proxy");
    }

    auto download_proxy_listener = create_listener();
    if (download_proxy_listener.socket.get() == -1) {
        return fail("cannot create proxied download test listener");
    }
    std::string download_proxy_error;
    std::string download_proxy_request;
    std::jthread download_proxy_server{[&] {
        if (!wait_for_connection(download_proxy_listener.socket.get())) {
            download_proxy_error =
                "timed out waiting for proxied download";
            return;
        }
        Socket connection{
            ::accept4(
                download_proxy_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
        if (connection.get() == -1 ||
            !receive_http_headers(
                connection.get(),
                download_proxy_request
            ) ||
            !send_http_ok(connection.get())) {
            download_proxy_error =
                "cannot serve proxied download request";
        }
    }};
    onedrive::http::CurlHttpClient download_proxy_client{{
        .url = "http://127.0.0.1:" +
               std::to_string(download_proxy_listener.port),
        .no_proxy = std::vector<std::string>{},
    }};
    const auto download_path = temporary.path() / "proxied-download";
    const auto download_proxy_response = download_proxy_client.download(
        {
            .url = "http://download-target.example/content",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
        },
        download_path
    );
    download_proxy_server.join();
    if (!download_proxy_error.empty()) {
        return fail(download_proxy_error);
    }
    if (!download_proxy_response ||
        download_proxy_response->status_code != 200 ||
        download_proxy_response->received_size != 2 ||
        !download_proxy_request.starts_with(
            "GET http://download-target.example/content HTTP/1.1"
        ) ||
        std::filesystem::file_size(download_path) != 2) {
        return fail("file download did not use the configured proxy");
    }

    auto https_listener = create_listener();
    if (https_listener.socket.get() == -1) {
        return fail("cannot create HTTPS proxy test listener");
    }
    bool tls_client_hello = false;
    std::string https_error;
    std::jthread https_server{[&] {
        if (!wait_for_connection(https_listener.socket.get())) {
            https_error = "timed out waiting for HTTPS proxy connection";
            return;
        }
        Socket connection{
            ::accept4(
                https_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
        std::array<std::byte, 1> first_byte{};
        if (connection.get() == -1 ||
            !receive_exact(connection.get(), first_byte)) {
            https_error = "cannot read HTTPS proxy handshake";
            return;
        }
        tls_client_hello = first_byte[0] == std::byte{0x16};
    }};

    onedrive::http::CurlHttpClient https_client{
        {.url = "https://127.0.0.1:" +
                std::to_string(https_listener.port)}
    };
    const auto https_response = https_client.perform({
        .url = "https://proxy-target.example/resource",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    https_server.join();
    if (!https_error.empty()) {
        return fail(https_error);
    }
    if (https_response) {
        return fail("incomplete HTTPS proxy unexpectedly completed a request");
    }
    if (!tls_client_hello) {
        return fail("HTTPS proxy connection did not start a TLS handshake");
    }

    const auto invalid_ca_path = temporary.path() / "invalid-proxy-ca.pem";
    write_file(invalid_ca_path, "not a certificate\n");
    auto ca_listener = create_listener();
    if (ca_listener.socket.get() == -1) {
        return fail("cannot create proxy CA test listener");
    }
    std::jthread ca_server{[&] {
        if (!wait_for_connection(ca_listener.socket.get())) {
            return;
        }
        const Socket connection{
            ::accept4(
                ca_listener.socket.get(),
                nullptr,
                nullptr,
                SOCK_CLOEXEC
            )
        };
    }};
    onedrive::http::CurlHttpClient invalid_ca_client{{
        .url = "https://127.0.0.1:" +
               std::to_string(ca_listener.port),
        .ca_file = invalid_ca_path,
    }};
    const auto invalid_ca_response = invalid_ca_client.perform({
        .url = "https://proxy-target.example/resource",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    ca_server.join();
    if (invalid_ca_response ||
        (!invalid_ca_response.error().message.contains("certificate") &&
         !invalid_ca_response.error().message.contains(
             invalid_ca_path.string()
         ))) {
        return fail("HTTPS proxy CA file was not applied");
    }

    return EXIT_SUCCESS;
}
