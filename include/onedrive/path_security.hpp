#pragma once

#include <filesystem>
#include <string_view>

namespace onedrive::detail {

[[nodiscard]] std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
);
void reject_symlink_components(
    const std::filesystem::path& path,
    std::string_view description
);

}  // namespace onedrive::detail
