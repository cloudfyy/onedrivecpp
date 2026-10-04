#include "onedrive/app/runtime_options.hpp"
#include "onedrive/config/config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto path =
        std::filesystem::temp_directory_path() / "onedrive-cpp-config-test.toml";
    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "directory = \"/tmp/OneDrive\"\n"
               << "drive_id = \"test-drive\"\n"
               << "dry_run = true\n"
               << "download_concurrency = 6\n"
               << "download_chunk_threshold_bytes = 4096\n"
               << "download_connect_timeout_seconds = 12\n"
               << "download_operation_timeout_seconds = 600\n"
               << "download_stall_timeout_seconds = 15\n"
               << "download_stall_minimum_bytes_per_second = 128\n"
               << "download_maximum_rate_bytes_per_second = 1048576\n"
               << "download_http_version = \"2\"\n"
               << "download_validation = \"relaxed\"\n"
               << "permissions = \"umask\"\n"
               << "[state]\n"
               << "directory = \"/tmp/onedrive-state\"\n"
               << "[auth]\n"
               << "application_id = \"test-application\"\n"
               << "tenant_id = \"test-tenant\"\n"
               << "endpoint = \"https://login.example.test\"\n"
               << "scopes = [\"Files.Read\", \"offline_access\"]\n"
               << "[graph]\n"
               << "endpoint = \"https://graph.example.test/v1.0\"\n"
               << "[graph.throttle]\n"
               << "maximum_retries = 7\n"
               << "initial_delay_seconds = 2\n"
               << "maximum_delay_seconds = 90\n"
               << "[filesystem]\n"
               << "metadata = \"database\"\n";
    }

    const auto config = onedrive::config::Config::load(path);
    std::filesystem::remove(path);
    const auto graph_options = onedrive::app::graph_options(config);

    if (config.sync_directory != "/tmp/OneDrive" ||
        config.state_directory != "/tmp/onedrive-state" ||
        config.drive_id != "test-drive" ||
        config.application_id != "test-application" ||
        config.azure_tenant_id != "test-tenant" ||
        config.auth_endpoint != "https://login.example.test" ||
        config.auth_scope != "Files.Read offline_access" ||
        config.graph_endpoint != "https://graph.example.test/v1.0" ||
        config.graph_maximum_throttle_retries != 7 ||
        config.graph_initial_throttle_delay != std::chrono::seconds{2} ||
        config.graph_maximum_throttle_delay != std::chrono::seconds{90} ||
        config.download_concurrency != 6 ||
        config.download_chunk_threshold_bytes != 4096 ||
        config.download_transport.connect_timeout !=
            std::chrono::seconds{12} ||
        config.download_transport.operation_timeout !=
            std::chrono::seconds{600} ||
        config.download_transport.low_speed_timeout !=
            std::chrono::seconds{15} ||
        config.download_transport.low_speed_limit_bytes_per_second != 128 ||
        config.download_transport.
                maximum_receive_speed_bytes_per_second !=
            1'048'576 ||
        config.download_transport.http_version !=
            onedrive::http::HttpVersion::http_2 ||
        config.download_validation !=
            onedrive::config::DownloadValidationMode::relaxed ||
        config.sync_permissions !=
            onedrive::config::SyncPermissionsMode::umask ||
        graph_options.download_transport != config.download_transport ||
        !graph_options.relaxed_download_validation ||
        graph_options.private_download_permissions ||
        config.filesystem_metadata !=
            onedrive::config::FilesystemMetadataMode::database ||
        !config.dry_run) {
        std::cerr << "configuration values were not parsed correctly\n";
        return EXIT_FAILURE;
    }
    const auto defaults = onedrive::config::Config::load(path);
    if (defaults.download_validation !=
            onedrive::config::DownloadValidationMode::strict ||
        defaults.auth_scope !=
            "User.Read Files.ReadWrite offline_access" ||
        !onedrive::config::has_auth_scope(
            defaults.auth_scope,
            "Files.ReadWrite"
        ) ||
        onedrive::config::has_auth_scope(
            defaults.auth_scope,
            "Files.ReadWrite.All"
        ) ||
        defaults.sync_permissions !=
            onedrive::config::SyncPermissionsMode::private_access ||
        onedrive::app::graph_options(defaults).
            relaxed_download_validation ||
        !onedrive::app::graph_options(defaults).
            private_download_permissions) {
        std::cerr << "secure synchronization defaults were not applied\n";
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_validation = \"unsafe\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download validation mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "permissions = \"shared\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid synchronization permissions were accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[graph.throttle]\n"
               << "maximum_retries = \"invalid\"\n";
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
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_http_version = \"3\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download HTTP version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_operation_timeout_seconds = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download operation timeout was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_stall_minimum_bytes_per_second = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download stall minimum was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_chunk_threshold_bytes = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download chunk threshold was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_concurrency = 17\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "excessive download concurrency was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "download_concurrency = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download concurrency was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[filesystem]\n"
               << "metadata = \"unsupported\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid filesystem metadata mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[graph.throttle]\n"
               << "initial_delay_seconds = 10\n"
               << "maximum_delay_seconds = 5\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid throttle delay range was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown TOML configuration key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "[sync]\n"
               << "drive_id = \"me\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "missing TOML configuration version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[auth]\n"
               << "scopes = [\"Files.Read\", 42]\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "non-string authentication scope was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n"
               << "[sync\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "malformed TOML configuration was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }
    std::filesystem::remove(path);
    return EXIT_SUCCESS;
}
