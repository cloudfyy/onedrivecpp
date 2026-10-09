#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"
#include "support/http.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using onedrive::test::TemporaryDirectory;

using FakeTransport = onedrive::test::QueuedHttpTransport;

using onedrive::test::fail;

template <typename Value>
bool has_error(
    const onedrive::auth::AuthResult<Value>& result,
    onedrive::auth::AuthErrorCode expected
) {
    return !result && result.error().code == expected;
}

onedrive::auth::DeviceAuthOptions test_options() {
    return {
        .application_id = "client id",
        .tenant_id = "test-tenant",
        .auth_endpoint = "https://login.example.test",
        .scope = "Files.ReadWrite offline_access",
    };
}

int test_queued_transport_contract() {
    FakeTransport transport{{}};
    const auto response = transport.perform({});
    const auto download = transport.download({}, {}, {}, {}, {}, {});
    if (response || download || transport.requests.size() != 1 ||
        response.error().message != "no fake response available" ||
        download.error().message != "download was not expected") {
        return fail("queued HTTP test transport contract was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_device_flow() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"device_code":"device-secret","user_code":"ABCD-EFGH",)"
                    R"("verification_uri":"https://microsoft.com/devicelogin",)"
                    R"("expires_in":900,"interval":1})",
        },
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"authorization_pending",)"
                    R"("error_description":"Waiting for the user"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"token_type":"Bearer","expires_in":3600,)"
                R"("access_token":"access-secret","refresh_token":"refresh-secret"})",
        },
    }};

    std::vector<std::chrono::seconds> sleeps;
    const auto fixed_time = std::chrono::steady_clock::now();
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    onedrive::auth::DeviceAuthClient client{
        &transport_proxy,
        test_options(),
        [&sleeps](std::chrono::seconds duration) {
            sleeps.push_back(duration);
        },
        [fixed_time] { return fixed_time; },
    };

    auto code = client.request_device_code();
    if (!code || code->user_code != "ABCD-EFGH") {
        return fail("device code response was not parsed");
    }
    auto tokens = client.poll_for_token(*code);
    if (!tokens || tokens->access_token != "access-secret" ||
        tokens->refresh_token != "refresh-secret") {
        return fail("device token response was not parsed");
    }
    if (sleeps !=
        std::vector{std::chrono::seconds{1}, std::chrono::seconds{1}}) {
        return fail("device flow did not wait before each token request");
    }
    if (transport.requests.size() != 3 ||
        transport.requests[0].url !=
            "https://login.example.test/test-tenant/oauth2/v2.0/devicecode" ||
        !transport.requests[0].body.contains("client_id=client%20id") ||
        !transport.requests[2].body.contains(
            "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
        )) {
        return fail("device flow request was not encoded correctly");
    }
    return EXIT_SUCCESS;
}

int test_refresh_and_token_store() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"expires_in":3600,"access_token":"new-access"})",
        },
    }};
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    onedrive::auth::DeviceAuthClient client{&transport_proxy, test_options()};
    auto tokens = client.refresh_access_token("existing-refresh");
    if (!tokens || tokens->refresh_token != "existing-refresh") {
        return fail(
            "refresh response did not preserve the existing refresh token"
        );
    }

    TemporaryDirectory temporary_directory;
    onedrive::auth::FileTokenStore store{temporary_directory.path()};
    store.save_refresh_token(tokens->refresh_token);
    store.save_refresh_token("rotated-refresh");
    const auto loaded = store.load_refresh_token();
    if (!loaded || *loaded != "rotated-refresh") {
        return fail("refresh token was not persisted");
    }

    const auto permissions =
        std::filesystem::status(store.path()).permissions();
    const auto expected = std::filesystem::perms::owner_read |
                          std::filesystem::perms::owner_write;
    if ((permissions & std::filesystem::perms::all) != expected) {
        return fail("refresh token permissions are not 0600");
    }
    std::filesystem::permissions(
        store.path(), expected | std::filesystem::perms::group_read
    );
    try {
        static_cast<void>(store.load_refresh_token());
        return fail("overly broad refresh token permissions were accepted");
    } catch (const std::runtime_error&) {
    }
    std::filesystem::permissions(store.path(), expected);
    constexpr std::size_t maximum_refresh_token_size = std::size_t{64} * 1024U;
    onedrive::test::write_file(
        store.path(), std::string(maximum_refresh_token_size + 1U, 'x')
    );
    try {
        static_cast<void>(store.load_refresh_token());
        return fail("oversized refresh token file was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains("65536 byte size limit")) {
            return fail("oversized refresh token reported the wrong error");
        }
    }
    store.save_refresh_token("rotated-refresh");
    if (!store.remove_refresh_token() || store.load_refresh_token()) {
        return fail("refresh token was not removed");
    }
    std::filesystem::create_symlink("/etc/passwd", store.path());
    try {
        static_cast<void>(store.load_refresh_token());
        return fail("symbolic-link refresh token was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

int test_declined_authorization() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"authorization_declined",)"
                    R"("error_description":"The user declined"})",
        },
    }};
    const auto fixed_time = std::chrono::steady_clock::now();
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    onedrive::auth::DeviceAuthClient client{
        &transport_proxy,
        test_options(),
        [](std::chrono::seconds) {},
        [fixed_time] { return fixed_time; },
    };
    const onedrive::auth::DeviceCode code{
        .device_code = "device-secret",
        .user_code = "ABCD-EFGH",
        .verification_uri = "https://microsoft.com/devicelogin",
        .message = {},
        .expires_in = std::chrono::seconds{900},
        .polling_interval = std::chrono::seconds{1},
    };

    const auto result = client.poll_for_token(code);
    if (result || result.error().code !=
                      onedrive::auth::AuthErrorCode::authorization_declined) {
        return fail("declined authorization was not reported");
    }
    return EXIT_SUCCESS;
}

int test_invalid_device_code_responses() {
    for (auto options : {
             onedrive::auth::DeviceAuthOptions{
                 .application_id = "",
                 .tenant_id = "tenant",
                 .auth_endpoint = "https://login.example.test",
                 .scope = "offline_access",
             },
             onedrive::auth::DeviceAuthOptions{
                 .application_id = "client",
                 .tenant_id = "",
                 .auth_endpoint = "https://login.example.test",
                 .scope = "offline_access",
             },
             onedrive::auth::DeviceAuthOptions{
                 .application_id = "client",
                 .tenant_id = "tenant",
                 .auth_endpoint = "http://login.example.test",
                 .scope = "offline_access",
             },
             onedrive::auth::DeviceAuthOptions{
                 .application_id = "client",
                 .tenant_id = "tenant",
                 .auth_endpoint = "https://login.example.test",
                 .scope = "Files.ReadWrite",
             },
         }) {
        FakeTransport transport{{}};
        onedrive::http::HttpTransport transport_proxy{
            onedrive::util::borrowed_proxy, transport
        };
        onedrive::auth::DeviceAuthClient client{&transport_proxy, options};
        if (!has_error(
                client.request_device_code(),
                onedrive::auth::AuthErrorCode::invalid_configuration
            )) {
            return fail("invalid device authorization options were accepted");
        }
    }

    struct ResponseCase {
        onedrive::http::HttpResult response;
        onedrive::auth::AuthErrorCode expected;
    };
    const std::vector<ResponseCase> cases{
        {
            std::unexpected(
                onedrive::http::HttpError{
                    .message = "transport unavailable",
                }
            ),
            onedrive::auth::AuthErrorCode::transport,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = "not-json",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 503,
                .body = R"({"error":"temporarily_unavailable",)"
                        R"("error_description":"try later"})",
            },
            onedrive::auth::AuthErrorCode::server,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = "{}",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"device_code":"","user_code":"code",)"
                        R"("verification_uri":"https://example.test",)"
                        R"("expires_in":900,"interval":1})",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
    };
    for (const auto& response_case : cases) {
        FakeTransport transport{{response_case.response}};
        onedrive::http::HttpTransport transport_proxy{
            onedrive::util::borrowed_proxy, transport
        };
        onedrive::auth::DeviceAuthClient client{
            &transport_proxy, test_options()
        };
        if (!has_error(client.request_device_code(), response_case.expected)) {
            return fail("invalid device code response was accepted");
        }
    }
    return EXIT_SUCCESS;
}

int test_polling_errors() {
    const onedrive::auth::DeviceCode code{
        .device_code = "device-secret",
        .user_code = "ABCD-EFGH",
        .verification_uri = "https://microsoft.com/devicelogin",
        .message = {},
        .expires_in = std::chrono::seconds{30},
        .polling_interval = std::chrono::seconds{1},
    };
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"slow_down"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"expired_token"})",
        },
    }};
    std::vector<std::chrono::seconds> sleeps;
    auto now = std::chrono::steady_clock::now();
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    onedrive::auth::DeviceAuthClient client{
        &transport_proxy,
        test_options(),
        [&](std::chrono::seconds duration) {
            sleeps.push_back(duration);
            now += duration;
        },
        [&] { return now; },
    };
    if (!has_error(
            client.poll_for_token(code), onedrive::auth::AuthErrorCode::expired
        ) ||
        sleeps != std::vector{
                      std::chrono::seconds{1},
                      std::chrono::seconds{6},
                  }) {
        return fail(
            "slow-down or expired token polling was handled incorrectly"
        );
    }

    FakeTransport timeout_transport{{}};
    onedrive::http::HttpTransport timeout_proxy{
        onedrive::util::borrowed_proxy, timeout_transport
    };
    auto timeout_now = std::chrono::steady_clock::now();
    onedrive::auth::DeviceAuthClient timeout_client{
        &timeout_proxy,
        test_options(),
        [&](std::chrono::seconds duration) { timeout_now += duration; },
        [&] { return timeout_now; },
    };
    auto expiring_code = code;
    expiring_code.expires_in = std::chrono::seconds{1};
    if (!has_error(
            timeout_client.poll_for_token(expiring_code),
            onedrive::auth::AuthErrorCode::expired
        ) ||
        !timeout_transport.requests.empty()) {
        return fail("device authorization deadline was not enforced");
    }

    FakeTransport server_transport{{
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"invalid_request",)"
                    R"("error_description":"bad device code"})",
        },
    }};
    onedrive::http::HttpTransport server_proxy{
        onedrive::util::borrowed_proxy, server_transport
    };
    const auto fixed_time = std::chrono::steady_clock::now();
    onedrive::auth::DeviceAuthClient server_client{
        &server_proxy,
        test_options(),
        [](std::chrono::seconds) {},
        [fixed_time] { return fixed_time; },
    };
    if (!has_error(
            server_client.poll_for_token(code),
            onedrive::auth::AuthErrorCode::server
        )) {
        return fail("unexpected polling server error was accepted");
    }

    FakeTransport missing_refresh_transport{{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"expires_in":3600,"access_token":"access"})",
        },
    }};
    onedrive::http::HttpTransport missing_refresh_proxy{
        onedrive::util::borrowed_proxy, missing_refresh_transport
    };
    onedrive::auth::DeviceAuthClient missing_refresh_client{
        &missing_refresh_proxy,
        test_options(),
        [](std::chrono::seconds) {},
        [fixed_time] { return fixed_time; },
    };
    if (!has_error(
            missing_refresh_client.poll_for_token(code),
            onedrive::auth::AuthErrorCode::invalid_response
        )) {
        return fail("device authorization accepted a missing refresh token");
    }
    return EXIT_SUCCESS;
}

int test_invalid_refresh_responses() {
    if (FakeTransport transport{{}}; [&] {
            onedrive::http::HttpTransport proxy{
                onedrive::util::borrowed_proxy, transport
            };
            onedrive::auth::DeviceAuthClient client{&proxy, test_options()};
            return !has_error(
                client.refresh_access_token(""),
                onedrive::auth::AuthErrorCode::invalid_configuration
            );
        }()) {
        return fail("empty refresh token was accepted");
    }

    struct ResponseCase {
        onedrive::http::HttpResult response;
        onedrive::auth::AuthErrorCode expected;
    };
    const std::vector<ResponseCase> cases{
        {
            std::unexpected(
                onedrive::http::HttpError{
                    .message = "transport unavailable",
                }
            ),
            onedrive::auth::AuthErrorCode::transport,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = "not-json",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 401,
                .body = R"({"error":"invalid_grant"})",
            },
            onedrive::auth::AuthErrorCode::server,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":""})",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":0,"access_token":"access"})",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
        {
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"access_token":"access"})",
            },
            onedrive::auth::AuthErrorCode::invalid_response,
        },
    };
    for (const auto& response_case : cases) {
        FakeTransport transport{{response_case.response}};
        onedrive::http::HttpTransport transport_proxy{
            onedrive::util::borrowed_proxy, transport
        };
        onedrive::auth::DeviceAuthClient client{
            &transport_proxy, test_options()
        };
        if (!has_error(
                client.refresh_access_token("refresh"), response_case.expected
            )) {
            return fail("invalid refresh token response was accepted");
        }
    }
    return EXIT_SUCCESS;
}

int test_cancellation() {
    FakeTransport transport{{}};
    onedrive::http::HttpTransport proxy{
        onedrive::util::borrowed_proxy, transport
    };
    const onedrive::auth::DeviceCode code{
        .device_code = "secret",
        .user_code = "code",
        .verification_uri = "https://example.test",
        .expires_in = std::chrono::seconds{900},
        .polling_interval = std::chrono::seconds{60},
    };
    std::stop_source stop;
    std::promise<void> entered_poll;
    auto entered = entered_poll.get_future();
    int clock_calls{0};
    onedrive::auth::DeviceAuthClient client{
        &proxy, test_options(), {}, [&] {
            if (++clock_calls == 2) {
                entered_poll.set_value();
            }
            return std::chrono::steady_clock::now();
        }
    };
    std::promise<onedrive::auth::AuthResult<onedrive::auth::OAuthTokens>>
        promise;
    auto result = promise.get_future();
    std::jthread worker{[&] {
        promise.set_value(client.poll_for_token(code, stop.get_token()));
    }};
    entered.wait();
    stop.request_stop();
    if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready ||
        !has_error(result.get(), onedrive::auth::AuthErrorCode::cancelled) ||
        !transport.requests.empty()) {
        return fail("default authorization wait did not stop promptly");
    }
    if (!has_error(
            client.request_device_code(stop.get_token()),
            onedrive::auth::AuthErrorCode::cancelled
        ) ||
        !transport.requests.empty()) {
        return fail("pre-cancelled device request performed HTTP");
    }

    FakeTransport cancelled_transport{{
        std::unexpected(
            onedrive::http::HttpError{
                .code = onedrive::http::HttpErrorCode::cancelled,
                .message = "HTTP cancelled",
            }
        ),
    }};
    onedrive::http::HttpTransport cancelled_proxy{
        onedrive::util::borrowed_proxy, cancelled_transport
    };
    std::stop_source active_stop;
    onedrive::auth::DeviceAuthClient cancelled_client{
        &cancelled_proxy, test_options()
    };
    if (!has_error(
            cancelled_client.request_device_code(active_stop.get_token()),
            onedrive::auth::AuthErrorCode::cancelled
        ) ||
        cancelled_transport.requests.size() != 1 ||
        cancelled_transport.requests.front().stop_token !=
            active_stop.get_token()) {
        return fail("HTTP cancellation was not mapped or token not forwarded");
    }

    std::stop_source sleep_stop;
    onedrive::auth::DeviceAuthClient sleep_client{
        &proxy, test_options(), [&sleep_stop](std::chrono::seconds) {
            sleep_stop.request_stop();
        }
    };
    if (!has_error(
            sleep_client.poll_for_token(code, sleep_stop.get_token()),
            onedrive::auth::AuthErrorCode::cancelled
        ) ||
        !transport.requests.empty()) {
        return fail("cancellation during polling wait performed HTTP");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_cancellation() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_queued_transport_contract();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_device_flow(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_refresh_and_token_store();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_declined_authorization();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_device_code_responses();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_polling_errors(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_invalid_refresh_responses();
}
