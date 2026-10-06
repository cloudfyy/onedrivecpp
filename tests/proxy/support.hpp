#include "onedrive/http/http_client.hpp"
#include "support/network.hpp"
#include "support/common.hpp"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace onedrive::test::proxy {

using onedrive::test::create_loopback_listener;
using onedrive::test::fail;
using onedrive::test::receive_exact;
using onedrive::test::receive_http_headers;
using onedrive::test::send_all;
using onedrive::test::Socket;
using onedrive::test::wait_for_connection;
using onedrive::test::write_file;

bool send_http_ok(int socket) {
    constexpr std::string_view response{"HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 2\r\n"
                                        "Connection: close\r\n"
                                        "\r\n"
                                        "OK"};
    return send_all(socket, std::as_bytes(std::span{response}));
}

bool proxy_password_rejected(
    const std::filesystem::path& path, std::string_view expected_error
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

struct ProxyFixture final {
    onedrive::test::TemporaryDirectory temporary;
    std::filesystem::path password_path{temporary.path() / "proxy-password"};

    void write_password() const {
        onedrive::test::write_file(password_path, "proxy-secret\r\n");
        std::filesystem::permissions(
            password_path,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );
    }
};

} // namespace onedrive::test::proxy
