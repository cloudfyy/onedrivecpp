#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"

#include <filesystem>
#include <cstdint>
#include <string_view>
#include <sys/stat.h>

namespace onedrive::util {

struct FilesystemIdentity {
    std::uint64_t device{0};
    std::uint64_t inode{0};
    bool operator==(const FilesystemIdentity&) const = default;
};

[[nodiscard]] constexpr FilesystemIdentity
filesystem_identity(const struct stat& status) noexcept {
    return {
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
    };
}

[[nodiscard]] std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
);
[[nodiscard]] std::filesystem::path
canonical_directory(const std::filesystem::path& path);
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
[[nodiscard]] struct stat inspect_owned_directory(
    int descriptor,
    const std::filesystem::path& path,
    std::string_view description
);
[[nodiscard]] struct stat inspect_owned_regular_file(
    int descriptor,
    const std::filesystem::path& path,
    std::string_view description
);
[[nodiscard]] struct stat secure_owned_directory(
    int descriptor,
    const std::filesystem::path& path,
    mode_t mode,
    std::string_view description
);
void secure_owned_directory(
    const std::filesystem::path& path, mode_t mode, std::string_view description
);
[[nodiscard]] struct stat secure_owned_regular_file(
    int descriptor,
    const std::filesystem::path& path,
    mode_t mode,
    std::string_view description
);

}  // namespace onedrive::util
