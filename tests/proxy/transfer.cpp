#include "support.hpp"

namespace {

using namespace onedrive::test::proxy;

int test_transfer() {
    ProxyFixture fixture;
    const auto& temporary = fixture.temporary;
    auto download_proxy_listener = create_loopback_listener();
    if (download_proxy_listener.socket.get() == -1) {
        return fail("cannot create proxied download test listener");
    }
    std::string download_proxy_error;
    std::string download_proxy_request;
    std::jthread download_proxy_server{[&] {
        if (!wait_for_connection(download_proxy_listener.socket.get())) {
            download_proxy_error = "timed out waiting for proxied download";
            return;
        }
        Socket connection{::accept4(
            download_proxy_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
        if (connection.get() == -1 ||
            !receive_http_headers(connection.get(), download_proxy_request) ||
            !send_http_ok(connection.get())) {
            download_proxy_error = "cannot serve proxied download request";
        }
    }};
    onedrive::http::CurlHttpClient download_proxy_client{{
        .url =
            "http://127.0.0.1:" + std::to_string(download_proxy_listener.port),
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

    auto https_listener = create_loopback_listener();
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
        Socket connection{::accept4(
            https_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
        std::array<std::byte, 1> first_byte{};
        if (connection.get() == -1 ||
            !receive_exact(connection.get(), first_byte)) {
            https_error = "cannot read HTTPS proxy handshake";
            return;
        }
        tls_client_hello = first_byte[0] == std::byte{0x16};
    }};

    onedrive::http::CurlHttpClient https_client{
        {.url = "https://127.0.0.1:" + std::to_string(https_listener.port)}
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
    auto ca_listener = create_loopback_listener();
    if (ca_listener.socket.get() == -1) {
        return fail("cannot create proxy CA test listener");
    }
    std::jthread ca_server{[&] {
        if (!wait_for_connection(ca_listener.socket.get())) {
            return;
        }
        const Socket connection{
            ::accept4(ca_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC)
        };
    }};
    onedrive::http::CurlHttpClient invalid_ca_client{{
        .url = "https://127.0.0.1:" + std::to_string(ca_listener.port),
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
         !invalid_ca_response.error().message.contains(invalid_ca_path.string())
        )) {
        return fail("HTTPS proxy CA file was not applied");
    }

    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_transfer();
}
