#pragma once

#include "onedrive/auth/device_auth.hpp"

#include <optional>

namespace onedrive::auth {

enum class AuthCodeState {
    created,
    awaiting_callback,
    authorized,
    exchanging_token,
    completed,
    cancelled,
    expired,
    failed,
};

// A single-use session, owned and driven by the authentication worker.
class AuthCodeSession {
public:
    using ClockFunction = DeviceAuthClient::ClockFunction;

    explicit AuthCodeSession(
        DeviceAuthOptions options,
        ClockFunction now = {},
        std::chrono::seconds timeout = std::chrono::minutes{5}
    );
    ~AuthCodeSession();
    AuthCodeSession(const AuthCodeSession&) = delete;
    AuthCodeSession& operator=(const AuthCodeSession&) = delete;

    [[nodiscard]] AuthCodeState state() const noexcept;
    [[nodiscard]] AuthResult<std::string>
    begin(const std::string& redirect_uri, std::stop_token stop_token = {});
    [[nodiscard]] AuthResult<void> check(std::stop_token stop_token = {});
    // Invalid/unrelated callbacks leave the session waiting. A server rejection
    // with the correct state terminates it.
    [[nodiscard]] AuthResult<void> accept_callback(
        const std::string& callback_uri, std::stop_token stop_token = {}
    );
    [[nodiscard]] AuthResult<OAuthTokens>
    exchange(const DeviceAuthClient& client, std::stop_token stop_token = {});
    void fail(
        AuthError error = {
            .code = AuthErrorCode::server,
            .message = "browser authorization failed"
        }
    );

private:
    AuthError terminate(AuthCodeState state, AuthError error);
    void clear_secrets();

    DeviceAuthOptions options_;
    ClockFunction now_;
    std::chrono::seconds timeout_;
    std::chrono::steady_clock::time_point deadline_;
    AuthCodeState state_{AuthCodeState::created};
    std::string redirect_uri_;
    std::string verifier_;
    std::string csrf_state_;
    std::string code_;
    std::optional<AuthError> error_;
};

} // namespace onedrive::auth
