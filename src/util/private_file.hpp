#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>

namespace onedrive::util {

inline constexpr std::size_t maximum_private_file_size =
    std::size_t{64} * 1024U;

struct PrivateFileRequirements {
    std::optional<uid_t> required_owner;
    std::optional<mode_t> exact_permissions;
    mode_t forbidden_permissions{0};
    std::string_view permission_requirement;
    std::size_t maximum_size{maximum_private_file_size};
    bool require_single_link{false};
};

[[nodiscard]] std::string read_private_file(
    const std::filesystem::path& path,
    std::string_view description,
    PrivateFileRequirements requirements
);

} // namespace onedrive::util
