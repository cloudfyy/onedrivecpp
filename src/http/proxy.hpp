#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace onedrive::http::detail {

[[nodiscard]] std::string read_proxy_password(
    const std::filesystem::path& path
);
[[nodiscard]] std::string join_proxy_bypass_list(
    const std::vector<std::string>& entries
);

}  // namespace onedrive::http::detail
