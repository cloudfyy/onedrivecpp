#include "support.hpp"

namespace {

using namespace onedrive::test::proxy;

int test_socks() {
    const onedrive::test::TemporaryDirectory temporary;
    using onedrive::test::write_file;
    auto socks_listener = create_loopback_listener();
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
    write_file(password_path, std::string(std::size_t{64} * 1024U + 1U, 'x'));
    if (!proxy_password_rejected(password_path, "65536 byte size limit")) {
        return fail("oversized proxy password file was accepted");
    }

    write_file(password_path, "proxy-secret\r\n");
    if (::chmod(password_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) ==
        -1) {
        return fail("cannot set insecure proxy password test permissions");
    }
    if (!proxy_password_rejected(password_path, "group or other permissions")) {
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
        Socket connection{::accept4(
            socks_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
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
                connection.get(), std::as_writable_bytes(std::span{methods})
            ) ||
            !methods.contains('\2')) {
            socks_error = "SOCKS5 proxy did not offer username/password mode";
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
            std::to_integer<std::size_t>(auth_header[1]), '\0'
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
            std::to_integer<std::size_t>(password_length[0]), '\0'
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
            request[0] != std::byte{5} || request[1] != std::byte{1} ||
            request[3] != std::byte{3}) {
            socks_error = "invalid SOCKS5 domain request";
            return;
        }
        const auto host_size = std::to_integer<std::size_t>(request[4]);
        std::string host(host_size, '\0');
        std::array<std::byte, 2> target_port{};
        if (!receive_exact(
                connection.get(), std::as_writable_bytes(std::span{host})
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
        .url = "socks5h://127.0.0.1:" + std::to_string(socks_listener.port),
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
            socks_response ? "SOCKS5 proxy returned an unexpected response"
                           : socks_response.error().message
        );
    }
    if (proxied_host != "proxy-target.example") {
        return fail("SOCKS5H did not delegate DNS resolution to the proxy");
    }
    if (proxy_username != "proxy-user" || proxy_password != "proxy-secret") {
        return fail("SOCKS5 proxy credentials were not applied correctly");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_socks();
}
