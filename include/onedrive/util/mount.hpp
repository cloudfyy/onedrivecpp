#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>

namespace onedrive::util {

class SyncMountUnavailableError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] bool is_mount_point(const std::filesystem::path& path);

void require_sync_mount(
    const std::filesystem::path& sync_data_directory,
    const std::optional<std::filesystem::path>& mount_point
);

}  // namespace onedrive::util
