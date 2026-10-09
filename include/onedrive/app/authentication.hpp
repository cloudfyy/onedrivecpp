#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/auth_code.hpp"

#include <functional>
#include <stop_token>

namespace onedrive::config {
struct Config;
}

namespace onedrive::events {
class Observer;
}

namespace onedrive::app {

class RuntimeFactory;

struct DeviceAuthorization {
    std::string user_code;
    std::string verification_uri;
    std::string message;
    std::chrono::seconds expires_in;
};

// Callbacks run synchronously; copy their data before posting to a UI thread.
using AuthorizationCallback = std::function<void(const DeviceAuthorization&)>;

using AuthCodeAuthorizationCallback = std::function<
    auth::AuthResult<void>(auth::AuthCodeSession&, std::stop_token)>;

struct AuthenticationResult {
    account::DriveIdentity identity;
    std::filesystem::path token_directory;
};

[[nodiscard]] auth::AuthResult<AuthenticationResult>
authenticate_auth_code_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    const AuthCodeAuthorizationCallback& authorization,
    std::stop_token stop_token = {}
);
[[nodiscard]] auth::AuthResult<AuthenticationResult>
authenticate_auth_code_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthCodeAuthorizationCallback& authorization,
    std::stop_token stop_token = {}
);

[[nodiscard]] auth::AuthResult<AuthenticationResult> authenticate_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token = {}
);
[[nodiscard]] auth::AuthResult<AuthenticationResult> authenticate_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token = {}
);

} // namespace onedrive::app
