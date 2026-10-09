#pragma once

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/config/config.hpp"

namespace onedrive::auth::detail {

[[nodiscard]] inline bool valid_options(const DeviceAuthOptions& options) {
    return !options.application_id.empty() && !options.tenant_id.empty() &&
           !options.scope.empty() &&
           config::has_auth_scope(options.scope, "offline_access") &&
           options.auth_endpoint.starts_with("https://");
}

} // namespace onedrive::auth::detail
