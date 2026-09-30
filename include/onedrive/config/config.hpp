#pragma once

#include <filesystem>
#include <string>

namespace onedrive::config {

struct Config {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string drive_id{"me"};
    std::string application_id;
    std::string azure_tenant_id{"common"};
    std::string auth_endpoint{"https://login.microsoftonline.com"};
    std::string auth_scope{
        "Files.ReadWrite Files.ReadWrite.All Sites.ReadWrite.All offline_access"
    };
    bool dry_run{false};

    [[nodiscard]] static Config defaults();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
};

}  // namespace onedrive::config
