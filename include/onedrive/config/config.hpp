#pragma once

#include <filesystem>
#include <string>

namespace onedrive::config {

struct Config {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string drive_id{"me"};
    bool dry_run{false};

    [[nodiscard]] static Config defaults();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
};

}  // namespace onedrive::config
