#include "onedrive/auth/device_auth.hpp"

#include "util/uri.hpp"
#include "auth/options.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <string_view>
#include <thread>
#include <mutex>
#include <utility>
#include <vector>

namespace onedrive::auth {
namespace {

using Json = nlohmann::json;
using FormValues = std::vector<std::pair<std::string_view, std::string_view>>;

using detail::valid_options;

AuthError invalid_configuration_error() {
    return {
        .code = AuthErrorCode::invalid_configuration,
        .message =
            "authentication requires application_id, tenant, an HTTPS endpoint, "
            "and an offline_access scope",
    };
}

AuthError cancelled_error() {
    return {
        .code = AuthErrorCode::cancelled,
        .message = "device authorization cancelled",
    };
}

AuthResult<Json> parse_json(const http::HttpResponse& response) {
    try {
        return Json::parse(response.body);
    } catch (const Json::exception& error) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_response,
            .message = "Microsoft authentication returned invalid JSON: " +
                       std::string{error.what()},
        });
    }
}

AuthResult<http::HttpResponse> post_form(
    const http::HttpTransport& transport,
    const std::string& url,
    const FormValues& values,
    const std::stop_token& stop_token = {}
) {
    if (stop_token.stop_requested()) {
        return std::unexpected(cancelled_error());
    }
    auto response = transport.perform(
        http::HttpRequest{
            .method = http::HttpMethod::post,
            .url = url,
            .headers = {"Content-Type: application/x-www-form-urlencoded"},
            .body = util::encode_uri_parameters(values),
            .stop_token = stop_token,
        }
    );
    if (stop_token.stop_requested() ||
        (!response &&
         response.error().code == http::HttpErrorCode::cancelled)) {
        return std::unexpected(cancelled_error());
    }
    if (!response) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::transport,
            .message = response.error().message,
        });
    }
    return *std::move(response);
}

std::string error_description(const Json& response, std::string_view fallback) {
    if (const auto iterator = response.find("error_description");
        iterator != response.end() && iterator->is_string()) {
        return iterator->get<std::string>();
    }
    return std::string{fallback};
}

AuthResult<OAuthTokens> parse_tokens(
    const Json& response,
    std::string fallback_refresh_token = {}
) {
    try {
        const std::string access_token = response.at("access_token").get<std::string>();
        const auto expires_in = response.at("expires_in").get<std::int64_t>();
        if (access_token.empty() || expires_in <= 0) {
            return std::unexpected(AuthError{
                .code = AuthErrorCode::invalid_response,
                .message = "Microsoft authentication returned an empty token or expiry",
            });
        }

        std::string refresh_token = std::move(fallback_refresh_token);
        if (const auto iterator = response.find("refresh_token");
            iterator != response.end() && iterator->is_string()) {
            refresh_token = iterator->get<std::string>();
        }
        if (refresh_token.empty()) {
            return std::unexpected(AuthError{
                .code = AuthErrorCode::invalid_response,
                .message = "Microsoft authentication did not return a refresh token",
            });
        }

        return OAuthTokens{
            .access_token = access_token,
            .refresh_token = std::move(refresh_token),
            .token_type = response.value("token_type", "Bearer"),
            .expires_at =
                std::chrono::system_clock::now() + std::chrono::seconds{expires_in},
        };
    } catch (const Json::exception& error) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_response,
            .message = "Microsoft token response is missing required data: " +
                       std::string{error.what()},
        });
    }
}

}  // namespace

DeviceAuthClient::DeviceAuthClient(
    gsl::not_null<const http::HttpTransport*> transport,
    DeviceAuthOptions options,
    SleepFunction sleep,
    ClockFunction now
)
    : transport_{transport},
      options_{std::move(options)},
      sleep_{std::move(sleep)},
      now_{now ? std::move(now) : ClockFunction{[] {
          return std::chrono::steady_clock::now();
      }}} {
}

AuthResult<DeviceCode>
DeviceAuthClient::request_device_code(const std::stop_token& stop_token) const {
    if (!valid_options(options_)) {
        return std::unexpected(invalid_configuration_error());
    }

    spdlog::debug("Requesting Microsoft device authorization code");
    auto response = post_form(
        *transport_,
        device_code_url(),
        {
            {"client_id", options_.application_id},
            {"scope", options_.scope},
        },
        stop_token
    );
    if (!response) {
        return std::unexpected(response.error());
    }

    auto json = parse_json(*response);
    if (!json) {
        return std::unexpected(json.error());
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::server,
            .message = error_description(*json, "device authorization request failed"),
        });
    }

    try {
        const auto expires_in = json->at("expires_in").get<std::int64_t>();
        const auto interval = json->value("interval", std::int64_t{5});
        DeviceCode result{
            .device_code = json->at("device_code").get<std::string>(),
            .user_code = json->at("user_code").get<std::string>(),
            .verification_uri = json->at("verification_uri").get<std::string>(),
            .message = json->value("message", ""),
            .expires_in = std::chrono::seconds{expires_in},
            .polling_interval = std::chrono::seconds{interval},
        };
        if (result.device_code.empty() || result.user_code.empty() ||
            result.verification_uri.empty() || expires_in <= 0 || interval <= 0) {
            return std::unexpected(AuthError{
                .code = AuthErrorCode::invalid_response,
                .message = "device authorization response contains invalid values",
            });
        }
        spdlog::debug(
            "Received Microsoft device authorization code with {} second expiry "
            "and {} second polling interval",
            expires_in,
            interval
        );
        return result;
    } catch (const Json::exception& error) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_response,
            .message = "device authorization response is missing required data: " +
                       std::string{error.what()},
        });
    }
}

AuthResult<OAuthTokens> DeviceAuthClient::poll_for_token(
    const DeviceCode& code, const std::stop_token& stop_token
) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(cancelled_error());
    }
    if (!valid_options(options_)) {
        return std::unexpected(invalid_configuration_error());
    }
    const auto deadline = now_() + code.expires_in;
    auto interval = code.polling_interval;
    std::size_t poll_attempt = 0;
    spdlog::debug("Waiting for Microsoft device authorization");

    while (now_() < deadline) {
        if (sleep_) {
            sleep_(interval);
        } else {
            std::mutex mutex;
            std::unique_lock lock{mutex};
            std::condition_variable_any condition;
            condition.wait_for(lock, stop_token, interval, [] {
                return false;
            });
        }
        if (stop_token.stop_requested()) {
            return std::unexpected(cancelled_error());
        }
        if (now_() >= deadline) {
            break;
        }

        ++poll_attempt;
        spdlog::trace(
            "Polling Microsoft device authorization (attempt {})",
            poll_attempt
        );
        auto response = post_form(
            *transport_,
            token_url(),
            {
                {"client_id", options_.application_id},
                {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                {"device_code", code.device_code},
            },
            stop_token
        );
        if (!response) {
            return std::unexpected(response.error());
        }

        auto json = parse_json(*response);
        if (!json) {
            return std::unexpected(json.error());
        }
        if (response->status_code >= 200 && response->status_code < 300) {
            spdlog::debug(
                "Microsoft device authorization completed after {} polls",
                poll_attempt
            );
            return parse_tokens(*json);
        }

        const std::string error = json->value("error", "");
        if (error == "authorization_pending") {
            spdlog::trace("Microsoft device authorization remains pending");
            continue;
        }
        if (error == "slow_down") {
            interval += std::chrono::seconds{5};
            spdlog::warn(
                "Microsoft device authorization requested slower polling; "
                "interval is now {} seconds",
                interval.count()
            );
            continue;
        }
        if (error == "authorization_declined") {
            return std::unexpected(AuthError{
                .code = AuthErrorCode::authorization_declined,
                .message = "authorization was declined by the user",
            });
        }
        if (error == "expired_token") {
            return std::unexpected(AuthError{
                .code = AuthErrorCode::expired,
                .message = "device authorization code expired",
            });
        }
        return std::unexpected(AuthError{
            .code = AuthErrorCode::server,
            .message = error_description(*json, "token request failed"),
        });
    }

    return std::unexpected(AuthError{
        .code = AuthErrorCode::expired,
        .message = "device authorization code expired",
    });
}

AuthResult<OAuthTokens> DeviceAuthClient::refresh_access_token(
    const std::string& refresh_token, const std::stop_token& stop_token
) const {
    if (!valid_options(options_) || refresh_token.empty()) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_configuration,
            .message =
                "valid authentication configuration and refresh token are required",
        });
    }

    auto response = post_form(
        *transport_,
        token_url(),
        {
            {"client_id", options_.application_id},
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"scope", options_.scope},
        },
        stop_token
    );
    if (!response) {
        return std::unexpected(response.error());
    }

    auto json = parse_json(*response);
    if (!json) {
        return std::unexpected(json.error());
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::server,
            .message = error_description(*json, "refresh token request failed"),
        });
    }
    return parse_tokens(*json, refresh_token);
}

AuthResult<OAuthTokens> DeviceAuthClient::exchange_authorization_code(
    const std::string& code,
    const std::string& redirect_uri,
    const std::string& verifier,
    const std::stop_token& stop_token
) const {
    if (!valid_options(options_)) {
        return std::unexpected(invalid_configuration_error());
    }
    if (code.empty() || redirect_uri.empty() || verifier.size() < 43 ||
        verifier.size() > 128 ||
        verifier.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
        ) != std::string::npos) {
        return std::unexpected(
            AuthError{
                .code = AuthErrorCode::invalid_configuration,
                .message = "authorization code exchange requires a code, "
                           "redirect URI, and a 43-128 character PKCE verifier",
            }
        );
    }
    auto response = post_form(
        *transport_,
        token_url(),
        {
            {"client_id", options_.application_id},
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", redirect_uri},
            {"code_verifier", verifier},
            {"scope", options_.scope},
        },
        stop_token
    );
    if (!response) {
        return std::unexpected(response.error());
    }
    const auto json = parse_json(*response);
    if (!json) {
        return std::unexpected(json.error());
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        return std::unexpected(
            AuthError{
                .code = AuthErrorCode::server,
                .message = error_description(
                    *json, "authorization code exchange failed"
                ),
            }
        );
    }
    return parse_tokens(*json);
}

std::string DeviceAuthClient::device_code_url() const {
    std::string endpoint = options_.auth_endpoint;
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return std::format(
        "{}/{}/oauth2/v2.0/devicecode", endpoint, options_.tenant_id
    );
}

std::string DeviceAuthClient::token_url() const {
    std::string endpoint = options_.auth_endpoint;
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return std::format(
        "{}/{}/oauth2/v2.0/token",
        endpoint,
        options_.tenant_id
    );
}

}  // namespace onedrive::auth
