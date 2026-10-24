#pragma once

#include "onedrive/auth/device_auth.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace onedrive::auth::detail {

[[nodiscard]] AuthError server_error(
    const nlohmann::json& response,
    std::string_view fallback,
    std::string_view configured_tenant
);

} // namespace onedrive::auth::detail
