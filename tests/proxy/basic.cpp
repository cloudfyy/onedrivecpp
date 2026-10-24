#include "support/progress.hpp"
#include "support.hpp"

namespace {

using namespace onedrive::test::proxy;

int test_basic() {
    ProxyFixture fixture;
    const auto& temporary = fixture.temporary;
    const auto& password_path = fixture.password_path;
    fixture.write_password();
    ::setenv("NO_PROXY", "*", 1);
    ::setenv("no_proxy", "*", 1);
    auto basic_proxy_listener = create_loopback_listener();
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
        Socket connection{::accept4(
            basic_proxy_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
        if (connection.get() == -1 ||
            !receive_http_headers(connection.get(), basic_proxy_request)) {
            basic_proxy_error = "cannot read HTTP Basic proxy request";
            return;
        }
        if (!send_http_ok(connection.get())) {
            basic_proxy_error = "cannot send HTTP Basic proxy response";
        }
    }};
    onedrive::http::CurlHttpClient basic_proxy_client{{
        .url = "http://127.0.0.1:" + std::to_string(basic_proxy_listener.port),
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
    if (!basic_proxy_response || basic_proxy_response->status_code != 200 ||
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

    auto bypass_listener = create_loopback_listener();
    if (bypass_listener.socket.get() == -1) {
        return fail("cannot create no-proxy test listener");
    }
    std::string bypass_error;
    std::jthread bypass_server{[&] {
        if (!wait_for_connection(bypass_listener.socket.get())) {
            bypass_error = "timed out waiting for direct bypass connection";
            return;
        }
        Socket connection{::accept4(
            bypass_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
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
        .url = "http://127.0.0.1:" + std::to_string(bypass_listener.port) +
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

    auto digest_listener = create_loopback_listener();
    if (digest_listener.socket.get() == -1) {
        return fail("cannot create inherited Digest proxy listener");
    }
    const auto inherited_url =
        "http://127.0.0.1:" + std::to_string(digest_listener.port);
    ::setenv("http_proxy", inherited_url.c_str(), 1);
    ::setenv("NO_PROXY", "*", 1);
    ::setenv("no_proxy", "*", 1);
    std::string digest_error;
    std::string digest_request;
    std::jthread digest_server{[&] {
        if (!wait_for_connection(digest_listener.socket.get())) {
            digest_error = "inherited proxy did not receive initial request";
            return;
        }
        {
            Socket connection{::accept4(
                digest_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
            )};
            std::string initial;
            constexpr std::string_view challenge =
                "HTTP/1.1 407 Proxy Authentication Required\r\n"
                "Proxy-Authenticate: Digest realm=\"offline-proxy\", "
                "nonce=\"deterministic-nonce\", qop=\"auth\"\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            if (connection.get() == -1 ||
                !receive_http_headers(connection.get(), initial) ||
                !send_all(connection.get(), std::as_bytes(std::span{challenge}))) {
                digest_error = "cannot send inherited proxy Digest challenge";
                return;
            }
        }
        if (!wait_for_connection(digest_listener.socket.get())) {
            digest_error = "inherited proxy authentication mode was not applied";
            return;
        }
        Socket connection{::accept4(
            digest_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
        )};
        if (connection.get() == -1 ||
            !receive_http_headers(connection.get(), digest_request) ||
            !send_http_ok(connection.get())) {
            digest_error = "cannot serve authenticated inherited proxy request";
        }
    }};
    onedrive::http::CurlHttpClient digest_client{{
        .no_proxy = std::vector<std::string>{},
        .username = "proxy-user",
        .password_file = password_path,
        .auth = onedrive::http::ProxyAuth::digest,
    }};
    const auto digest_response = digest_client.perform({
        .url = "http://digest-target.example/resource",
        .connect_timeout = std::chrono::seconds{2},
        .operation_timeout = std::chrono::seconds{5},
    });
    digest_server.join();
    if (!digest_error.empty()) {
        return fail(digest_error);
    }
    if (!digest_response || digest_response->status_code != 200 ||
        !digest_request.starts_with("GET http://digest-target.example/resource HTTP/1.1") ||
        !digest_request.contains("Proxy-Authorization: Digest ") ||
        !digest_request.contains("username=\"proxy-user\"") ||
        !digest_request.contains("nonce=\"deterministic-nonce\"")) {
        return fail("Digest credentials for an environment-inherited proxy were lost");
    }

    ::setenv("http_proxy", "http://127.0.0.1:1", 1);
    for (const bool empty_url : {true, false}) {
        auto direct_listener = create_loopback_listener();
        if (direct_listener.socket.get() == -1) {
            return fail("cannot create direct override listener");
        }
        std::string direct_error;
        std::string direct_request;
        std::jthread direct_server{[&] {
            if (!wait_for_connection(direct_listener.socket.get())) {
                direct_error = "empty proxy/inherited bypass did not connect directly";
                return;
            }
            Socket connection{::accept4(
                direct_listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC
            )};
            if (connection.get() == -1 ||
                !receive_http_headers(connection.get(), direct_request) ||
                !send_http_ok(connection.get())) {
                direct_error = "cannot serve direct override request";
            }
        }};
        onedrive::http::ProxyOptions options;
        if (empty_url) {
            options.url = "";
            options.no_proxy = std::vector<std::string>{};
        }
        onedrive::http::CurlHttpClient direct_client{options};
        const auto response = direct_client.perform({
            .url = "http://127.0.0.1:" + std::to_string(direct_listener.port) + "/direct",
            .connect_timeout = std::chrono::seconds{2},
            .operation_timeout = std::chrono::seconds{5},
        });
        direct_server.join();
        if (!direct_error.empty()) {
            return fail(direct_error);
        }
        if (!response || response->status_code != 200 ||
            !direct_request.starts_with("GET /direct HTTP/1.1")) {
            return fail("explicit empty proxy or inherited bypass used the proxy");
        }
    }

    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{1};
    return progress.finish(progress.run("basic", test_basic));
}
