#include "onedrive/config/config.hpp"

#include <chrono>
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
               << "graph_maximum_throttle_retries=7\n"
               << "graph_initial_throttle_delay_seconds=2\n"
               << "graph_maximum_throttle_delay_seconds=90\n"
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
        config.graph_maximum_throttle_retries != 7 ||
        config.graph_initial_throttle_delay != std::chrono::seconds{2} ||
        config.graph_maximum_throttle_delay != std::chrono::seconds{90} ||
        !config.dry_run) {
        std::cerr << "configuration values were not parsed correctly\n";
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "graph_maximum_throttle_retries=invalid\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid throttle retry count was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "graph_initial_throttle_delay_seconds=10\n"
               << "graph_maximum_throttle_delay_seconds=5\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid throttle delay range was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }
    std::filesystem::remove(path);
    return EXIT_SUCCESS;
}
