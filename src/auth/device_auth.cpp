#include "onedrive/auth/device_auth.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace onedrive::auth {
namespace {

using Json = nlohmann::json;
using FormValues = std::vector<std::pair<std::string_view, std::string_view>>;

bool has_offline_access(std::string_view scope) {
    std::istringstream values{std::string{scope}};
    std::string value;
    while (values >> value) {
        if (value == "offline_access") {
            return true;
        }
    }
    return false;
}

bool valid_options(const DeviceAuthOptions& options) {
    return !options.application_id.empty() && !options.tenant_id.empty() &&
           !options.scope.empty() && has_offline_access(options.scope) &&
           options.auth_endpoint.starts_with("https://");
}

AuthError invalid_configuration_error() {
    return {
        .code = AuthErrorCode::invalid_configuration,
        .message =
            "authentication requires application_id, tenant, an HTTPS endpoint, "
            "and an offline_access scope",
    };
}

std::string percent_encode(std::string_view value) {
    constexpr std::string_view hex{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size());
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const bool unreserved =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.' || character == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4U]);
            encoded.push_back(hex[character & 0x0FU]);
        }
    }
    return encoded;
}

std::string encode_form(const FormValues& values) {
    std::string body;
    for (const auto& [name, value] : values) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += percent_encode(name);
        body.push_back('=');
        body += percent_encode(value);
    }
    return body;
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
    const FormValues& values
) {
    auto response = transport.perform(http::HttpRequest{
        .method = http::HttpMethod::post,
        .url = url,
        .headers = {"Content-Type: application/x-www-form-urlencoded"},
        .body = encode_form(values),
    });
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
    const http::HttpTransport& transport,
    DeviceAuthOptions options,
    SleepFunction sleep,
    ClockFunction now
)
    : transport_{transport},
      options_{std::move(options)},
      sleep_{
          sleep ? std::move(sleep) :
                  SleepFunction{[](std::chrono::seconds duration) {
                      std::this_thread::sleep_for(duration);
                  }}
      },
      now_{
          now ? std::move(now) :
                ClockFunction{[] { return std::chrono::steady_clock::now(); }}
      } {}

AuthResult<DeviceCode> DeviceAuthClient::request_device_code() const {
    if (!valid_options(options_)) {
        return std::unexpected(invalid_configuration_error());
    }

    auto response = post_form(
        transport_,
        device_code_url(),
        {
            {"client_id", options_.application_id},
            {"scope", options_.scope},
        }
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
        return result;
    } catch (const Json::exception& error) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_response,
            .message = "device authorization response is missing required data: " +
                       std::string{error.what()},
        });
    }
}

AuthResult<OAuthTokens> DeviceAuthClient::poll_for_token(const DeviceCode& code) const {
    if (!valid_options(options_)) {
        return std::unexpected(invalid_configuration_error());
    }
    const auto deadline = now_() + code.expires_in;
    auto interval = code.polling_interval;

    while (now_() < deadline) {
        auto response = post_form(
            transport_,
            token_url(),
            {
                {"client_id", options_.application_id},
                {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                {"device_code", code.device_code},
            }
        );
        if (!response) {
            return std::unexpected(response.error());
        }

        auto json = parse_json(*response);
        if (!json) {
            return std::unexpected(json.error());
        }
        if (response->status_code >= 200 && response->status_code < 300) {
            return parse_tokens(*json);
        }

        const std::string error = json->value("error", "");
        if (error == "authorization_pending") {
            sleep_(interval);
            continue;
        }
        if (error == "slow_down") {
            interval += std::chrono::seconds{5};
            sleep_(interval);
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
    const std::string& refresh_token
) const {
    if (!valid_options(options_) || refresh_token.empty()) {
        return std::unexpected(AuthError{
            .code = AuthErrorCode::invalid_configuration,
            .message =
                "valid authentication configuration and refresh token are required",
        });
    }

    auto response = post_form(
        transport_,
        token_url(),
        {
            {"client_id", options_.application_id},
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"scope", options_.scope},
        }
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

std::string DeviceAuthClient::device_code_url() const {
    std::string endpoint = options_.auth_endpoint;
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return std::format(
        "{}/{}/oauth2/v2.0/devicecode",
        endpoint,
        options_.tenant_id
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
