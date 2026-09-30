#include "onedrive/config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "onedrive-cpp-config-test.conf";
    {
        std::ofstream output{path};
        output << "sync_directory=/tmp/OneDrive\n"
               << "state_directory=/tmp/onedrive-state\n"
               << "drive_id=test-drive\n"
               << "application_id=test-application\n"
               << "azure_tenant_id=test-tenant\n"
               << "auth_endpoint=https://login.example.test\n"
               << "auth_scope=Files.Read offline_access\n"
               << "dry_run=true\n";
    }

    const auto config = onedrive::config::Config::load(path);
    std::filesystem::remove(path);

    if (config.sync_directory != "/tmp/OneDrive" ||
        config.state_directory != "/tmp/onedrive-state" ||
        config.drive_id != "test-drive" ||
        config.application_id != "test-application" ||
        config.azure_tenant_id != "test-tenant" ||
        config.auth_endpoint != "https://login.example.test" ||
        config.auth_scope != "Files.Read offline_access" ||
        !config.dry_run) {
        std::cerr << "configuration values were not parsed correctly\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
