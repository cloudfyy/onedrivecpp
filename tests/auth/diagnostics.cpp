#include "diagnostics_cases.hpp"
#include "auth/error.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "support/common.hpp"
#include "support/http.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace onedrive::test::auth::diagnostics_cases {
namespace {

namespace core = onedrive::auth;
using onedrive::test::fail;
using namespace std::chrono_literals;

enum class Endpoint { device_code, poll, exchange, refresh };

core::DeviceAuthOptions options() {
    return {
        .application_id = "diagnostic-client",
        .tenant_id = "organizations",
        .auth_endpoint = "https://login.example.test",
    };
}

core::DeviceCode device_code() {
    return {
        .device_code = "device-code-secret",
        .user_code = "user-code",
        .verification_uri = "https://login.example.test/device",
        .expires_in = 30s,
        .polling_interval = 1s,
    };
}

template <typename Value>
core::AuthError require_error(core::AuthResult<Value> result) {
    if (result) {
        throw std::runtime_error{"authentication accepted an error response"};
    }
    return std::move(result.error());
}

core::AuthError
request_error(const core::DeviceAuthClient& client, Endpoint endpoint) {
    switch (endpoint) {
    case Endpoint::device_code:
        return require_error(client.request_device_code());
    case Endpoint::poll:
        return require_error(client.poll_for_token(device_code()));
    case Endpoint::exchange:
        return require_error(client.exchange_authorization_code(
            "authorization-code-secret",
            "http://localhost:12345/",
            std::string(43, 'p')
        ));
    case Endpoint::refresh:
        return require_error(
            client.refresh_access_token("saved-refresh-secret")
        );
    }
    throw std::logic_error{"unexpected authentication endpoint"};
}

} // namespace

int test_enterprise_error_diagnostics() {
    struct Case {
        const char* body;
        const char* oauth;
        std::vector<std::uint64_t> codes;
        const char* hint;
    };
    const Case cases[]{
        {R"({"error":"invalid_grant","error_description":"provider consent failure","error_codes":[65001]})",
         "invalid_grant",
         {65001},
         "Consent is required"},
        {R"({"error":"access_denied","error_codes":[90094]})",
         "access_denied",
         {90094},
         "Administrator consent is required"},
        {R"({"error":"invalid_grant","error_codes":[53003,50076]})",
         "invalid_grant",
         {53003, 50076},
         "review Conditional Access"},
        {R"({"error":"invalid_grant","error_codes":[530036]})",
         "invalid_grant",
         {530036},
         "authentication flow used by this grant"},
        {R"({"error":"invalid_grant","error_codes":[700082]})",
         "invalid_grant",
         {700082},
         "invalid, expired or revoked"},
        {R"({"error":"invalid_grant"})",
         "invalid_grant",
         {},
         "invalid, expired or revoked"},
        {R"({"error":"consent_required"})",
         "consent_required",
         {},
         "Consent is required"},
        {R"({"error":"access_denied","error_codes":[65004]})",
         "access_denied",
         {65004},
         "Consent was declined"},
        {R"({"error":"access_denied"})",
         "access_denied",
         {},
         "this error alone does not identify the policy"},
        {R"({"error":"unauthorized_client"})",
         "unauthorized_client",
         {},
         "Allow public client flows"},
        {R"({"error":"unsupported_grant_type"})",
         "unsupported_grant_type",
         {},
         "Do not add a client secret"},
        {R"({"error":"interaction_required","claims":"claims-secret","access_token":"unverified-token-secret","refresh_token":"response-refresh-secret","id_token":"id-token-secret"})",
         "interaction_required",
         {},
         "Interactive sign-in is required"},
        {R"({"error":"login_required"})",
         "login_required",
         {},
         "Interactive sign-in is required"},
        {R"({"error":"temporarily_unavailable","error_codes":[65001]})",
         "temporarily_unavailable",
         {65001},
         "Try again later"},
        {R"({"error":"server_error","error_codes":[530036]})",
         "server_error",
         {530036},
         "Try again later"},
        {R"({"error":"future_error","error_description":"future provider failure","error_codes":[99999999]})",
         "future_error",
         {99999999},
         "future provider failure"},
        {R"({"error":"future_error","error_description":"large diagnostic","error_codes":[18446744073709551615]})",
         "future_error",
         {std::numeric_limits<std::uint64_t>::max()},
         "large diagnostic"},
    };
    const auto now = std::chrono::steady_clock::now();
    for (const auto& scenario : cases) {
        for (const auto endpoint :
             {Endpoint::device_code,
              Endpoint::poll,
              Endpoint::exchange,
              Endpoint::refresh}) {
            QueuedHttpTransport transport{
                {http::HttpResponse{.status_code = 400, .body = scenario.body}}
            };
            http::HttpTransport proxy{util::borrowed_proxy, transport};
            core::DeviceAuthClient client{
                &proxy, options(), [](auto) {}, [now] { return now; }
            };
            const auto error = request_error(client, endpoint);
            const auto message = core::describe_error(error);
            if (error.code != core::AuthErrorCode::server ||
                error.oauth_error != scenario.oauth ||
                error.server_error_codes != scenario.codes ||
                error.configured_tenant != "organizations" ||
                !message.contains(scenario.hint) ||
                !message.contains(
                    "Configured tenant/selector (not a verified tenant ID): "
                    "organizations"
                ) ||
                transport.requests.size() != 1 ||
                transport.requests.front().body.contains("Sites.") ||
                transport.requests.front().body.contains(
                    "Files.ReadWrite.All"
                )) {
                return fail(
                    "enterprise authentication lost diagnostics, context or "
                    "least-privilege behavior"
                );
            }
            for (const auto secret :
                 {"claims-secret",
                  "id-token-secret",
                  "unverified-token-secret",
                  "response-refresh-secret",
                  "saved-refresh-secret",
                  "authorization-code-secret",
                  "device-code-secret"}) {
                if (message.contains(secret)) {
                    return fail(
                        "authentication diagnostics disclosed a protocol secret"
                    );
                }
            }
            for (const auto code : scenario.codes) {
                if (!message.contains("AADSTS" + std::to_string(code))) {
                    return fail(
                        "authentication output lost an STS diagnostic code"
                    );
                }
            }
            if (error.oauth_error == "future_error" &&
                (message.contains("Consent is required") ||
                 message.contains("expired or revoked"))) {
                return fail(
                    "unknown diagnostics were assigned a guessed cause"
                );
            }
        }
    }
    const core::AuthError legacy{
        .code = core::AuthErrorCode::server, .message = "legacy error"
    };
    if (core::describe_error(legacy) != "legacy error") {
        return fail("authentication diagnostics changed unannotated errors");
    }
    return EXIT_SUCCESS;
}

int test_error_response_fallbacks() {
    for (const auto& [endpoint, fallback] :
         {std::pair{
              Endpoint::device_code, "device authorization request failed"
          },
          std::pair{Endpoint::poll, "token request failed"},
          std::pair{Endpoint::exchange, "authorization code exchange failed"},
          std::pair{Endpoint::refresh, "refresh token request failed"}}) {
        for (const auto* body :
             {"{}",
              R"({"error":"","error_description":"","error_codes":[]})",
              R"({"error":"future_error","error_description":""})"}) {
            QueuedHttpTransport transport{
                {http::HttpResponse{.status_code = 400, .body = body}}
            };
            http::HttpTransport proxy{util::borrowed_proxy, transport};
            core::DeviceAuthClient client{&proxy, options(), [](auto) {}};
            const auto error = request_error(client, endpoint);
            if (error.code != core::AuthErrorCode::server ||
                error.message != fallback ||
                !error.server_error_codes.empty() ||
                !core::describe_error(error).starts_with(fallback) ||
                transport.requests.size() != 1) {
                return fail(
                    "missing diagnostic fields lost the endpoint fallback"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_signed_diagnostic_code() {
    const auto code = std::numeric_limits<std::int64_t>::max();
    const auto error = core::detail::server_error(
        nlohmann::json{
            {"error", "future_error"},
            {"error_description", "signed diagnostic"},
            {"error_codes", nlohmann::json::array({code})},
        },
        "fallback",
        "organizations"
    );
    if (error.code != core::AuthErrorCode::server ||
        error.message != "signed diagnostic" ||
        error.oauth_error != "future_error" ||
        error.configured_tenant != "organizations" ||
        error.server_error_codes !=
            std::vector{static_cast<std::uint64_t>(code)} ||
        !core::describe_error(error).contains(
            "AADSTS" + std::to_string(code)
        )) {
        return fail("positive signed diagnostic was rejected or truncated");
    }
    return EXIT_SUCCESS;
}

int test_malformed_error_diagnostics() {
    const auto now = std::chrono::steady_clock::now();
    for (const auto* body : {
             "[]",
             "null",
             R"({"error":42})",
             R"({"error_description":false})",
             R"({"error_codes":"65001"})",
             R"({"error_codes":[-1]})",
             R"({"error_codes":[0]})",
             R"({"error_codes":[65001.0]})",
             R"({"error_codes":["65001"]})",
             R"({"error_codes":[{}]})",
             R"({"error_codes":null})",
             R"({"error_codes":[null]})",
             R"({"error_codes":[65001,-1]})",
             R"({"error_codes":[18446744073709551616]})",
         }) {
        for (const auto endpoint :
             {Endpoint::device_code,
              Endpoint::poll,
              Endpoint::exchange,
              Endpoint::refresh}) {
            QueuedHttpTransport transport{
                {http::HttpResponse{.status_code = 400, .body = body}}
            };
            http::HttpTransport proxy{util::borrowed_proxy, transport};
            core::DeviceAuthClient client{
                &proxy, options(), [](auto) {}, [now] { return now; }
            };
            const auto error = request_error(client, endpoint);
            if (error.code != core::AuthErrorCode::invalid_response ||
                !error.message.contains("invalid error response field") ||
                transport.requests.size() != 1) {
                return fail(
                    "malformed authentication diagnostics escaped typed error "
                    "handling"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_diagnostic_codes_do_not_control_polling() {
    const auto now = std::chrono::steady_clock::now();
    QueuedHttpTransport transport{{
        http::HttpResponse{
            .status_code = 400,
            .body =
                R"({"error":"authorization_pending","error_codes":[530036,90094]})"
        },
        http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"expires_in":3600,"access_token":"access","refresh_token":"refresh"})"
        },
        http::HttpResponse{
            .status_code = 400,
            .body = R"({"error":"invalid_grant","error_codes":[70016]})"
        },
    }};
    http::HttpTransport proxy{util::borrowed_proxy, transport};
    core::DeviceAuthClient client{
        &proxy, options(), [](auto) {}, [now] { return now; }
    };
    if (!client.poll_for_token(device_code()) ||
        transport.requests.size() != 2) {
        return fail("diagnostic numbers overrode OAuth authorization_pending");
    }
    const auto error = require_error(client.poll_for_token(device_code()));
    if (error.code != core::AuthErrorCode::server ||
        error.oauth_error != "invalid_grant" ||
        transport.requests.size() != 3) {
        return fail(
            "an STS diagnostic code incorrectly restarted device polling"
        );
    }
    for (const auto& [oauth, expected] :
         {std::pair{
              "authorization_declined",
              core::AuthErrorCode::authorization_declined
          },
          std::pair{"expired_token", core::AuthErrorCode::expired}}) {
        QueuedHttpTransport terminal_transport{{
            http::HttpResponse{
                .status_code = 400,
                .body = R"({"error":"slow_down","error_codes":[530036]})",
            },
            http::HttpResponse{
                .status_code = 400,
                .body = std::string{R"({"error":")"} + oauth +
                        R"(","error_codes":[65001]})",
            },
        }};
        http::HttpTransport terminal_proxy{
            util::borrowed_proxy, terminal_transport
        };
        std::vector<std::chrono::seconds> waits;
        core::DeviceAuthClient terminal_client{
            &terminal_proxy,
            options(),
            [&](auto delay) { waits.push_back(delay); },
            [now] { return now; },
        };
        const auto terminal =
            require_error(terminal_client.poll_for_token(device_code()));
        if (terminal.code != expected || terminal.oauth_error != oauth ||
            waits != std::vector{1s, 6s} ||
            terminal_transport.requests.size() != 2) {
            return fail(
                "diagnostic codes changed slow_down or terminal polling "
                "behavior"
            );
        }
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::auth::diagnostics_cases
