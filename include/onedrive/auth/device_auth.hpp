#pragma once

#include "onedrive/http/http_client.hpp"

#include <gsl/pointers>

#include <chrono>
#include <expected>
#include <functional>
#include <string>
#include <stop_token>

namespace onedrive::auth {

struct DeviceAuthOptions {
    std::string application_id;
    std::string tenant_id{"common"};
    std::string auth_endpoint{"https://login.microsoftonline.com"};
    std::string scope{
        "User.Read Files.ReadWrite offline_access"
    };
};

struct DeviceCode {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string message;
    std::chrono::seconds expires_in;
    std::chrono::seconds polling_interval;
};

struct OAuthTokens {
    std::string access_token;
    std::string refresh_token;
    std::string token_type;
    std::chrono::system_clock::time_point expires_at;
};

enum class AuthErrorCode {
    invalid_configuration,
    transport,
    invalid_response,
    authorization_declined,
    expired,
    server,
    cancelled,
};

struct AuthError {
    AuthErrorCode code;
    std::string message;
};

template <typename Value>
using AuthResult = std::expected<Value, AuthError>;

class DeviceAuthClient {
public:
    using SleepFunction = std::function<void(std::chrono::seconds)>;
    using ClockFunction = std::function<std::chrono::steady_clock::time_point()>;

    DeviceAuthClient(
        gsl::not_null<const http::HttpTransport*> transport,
        DeviceAuthOptions options,
        SleepFunction sleep = {},
        ClockFunction now = {}
    );

    [[nodiscard]] AuthResult<DeviceCode>
    request_device_code(std::stop_token stop_token = {}) const;
    [[nodiscard]] AuthResult<OAuthTokens> poll_for_token(
        const DeviceCode& code, std::stop_token stop_token = {}
    ) const;
    [[nodiscard]] AuthResult<OAuthTokens> refresh_access_token(
        const std::string& refresh_token
    ) const;

private:
    gsl::not_null<const http::HttpTransport*> transport_;
    DeviceAuthOptions options_;
    SleepFunction sleep_;
    ClockFunction now_;

    [[nodiscard]] std::string device_code_url() const;
    [[nodiscard]] std::string token_url() const;
};

}  // namespace onedrive::auth
