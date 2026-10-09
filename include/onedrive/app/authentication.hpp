#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"

#include <functional>
#include <stop_token>

namespace onedrive::config {
struct Config;
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

struct AuthenticationResult {
    account::DriveIdentity identity;
    std::filesystem::path token_directory;
};

[[nodiscard]] auth::AuthResult<AuthenticationResult> authenticate_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token = {}
);

} // namespace onedrive::app
