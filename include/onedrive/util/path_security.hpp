#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"

#include <filesystem>
#include <string_view>
#include <sys/stat.h>

namespace onedrive::util {

[[nodiscard]] std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
);
[[nodiscard]] bool path_contains(
    const std::filesystem::path& parent,
    const std::filesystem::path& child
);
void reject_symlink_components(
    const std::filesystem::path& path,
    std::string_view description
);
[[nodiscard]] UniqueFD open_path_no_symlinks(
    const std::filesystem::path& path,
    int flags,
    mode_t mode = 0
);

}  // namespace onedrive::util
