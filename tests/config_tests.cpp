#include "onedrive/app/runtime_options.hpp"
#include "onedrive/config/config.hpp"

#include <array>
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
        output << "config_version = 2\n"
               << "[sync]\n"
               << "directory = \"/tmp/OneDrive\"\n"
               << "drive_id = \"test-drive\"\n"
               << "sync_list = \"rules/sync_list\"\n"
               << "sync_root_files = true\n"
               << "local_conflict = \"backup\"\n"
               << "dry_run = true\n"
               << "permissions = \"umask\"\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test:8443\"\n"
               << "no_proxy = [\"localhost\", \".internal.test\"]\n"
               << "username = \"proxy-user\"\n"
               << "password_file = \"secrets/proxy-password\"\n"
               << "auth = \"digest\"\n"
               << "ca_file = \"certificates/proxy-ca.pem\"\n"
               << "[transfer]\n"
               << "connect_timeout_seconds = 12\n"
               << "operation_timeout_seconds = 600\n"
               << "stall_timeout_seconds = 15\n"
               << "stall_minimum_bytes_per_second = 128\n"
               << "http_version = \"2\"\n"
               << "ip_version = \"6\"\n"
               << "order = \"size_dsc\"\n"
               << "[download]\n"
               << "concurrency = 6\n"
               << "maximum_retries = 3\n"
               << "chunk_threshold_bytes = 4096\n"
               << "checkpoint_interval_bytes = 1024\n"
               << "maximum_rate_bytes_per_second = 1048576\n"
               << "validation = \"relaxed\"\n"
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
        config.sync_list !=
            std::optional<std::filesystem::path>{
                path.parent_path() / "rules/sync_list"
            } ||
        !config.sync_root_files ||
        config.application_id != "test-application" ||
        config.azure_tenant_id != "test-tenant" ||
        config.auth_endpoint != "https://login.example.test" ||
        config.auth_scope != "Files.Read offline_access" ||
        config.graph_endpoint != "https://graph.example.test/v1.0" ||
        config.graph_maximum_throttle_retries != 7 ||
        config.graph_initial_throttle_delay != std::chrono::seconds{2} ||
        config.graph_maximum_throttle_delay != std::chrono::seconds{90} ||
        config.transfer_order !=
            onedrive::config::TransferOrder::size_descending ||
        config.download_concurrency != 6 ||
        config.download_maximum_retries != 3 ||
        config.download_chunk_threshold_bytes != 4096 ||
        config.download_checkpoint_interval_bytes != 1024 ||
        config.transfer_transport.connect_timeout !=
            std::chrono::seconds{12} ||
        config.transfer_transport.operation_timeout !=
            std::chrono::seconds{600} ||
        config.transfer_transport.low_speed_timeout !=
            std::chrono::seconds{15} ||
        config.transfer_transport.low_speed_limit_bytes_per_second != 128 ||
        config.download_maximum_rate_bytes_per_second !=
            1'048'576 ||
        config.transfer_transport.http_version !=
            onedrive::http::HttpVersion::http_2 ||
        config.transfer_transport.ip_version !=
            onedrive::http::IpVersion::ipv6 ||
        config.download_validation !=
            onedrive::config::DownloadValidationMode::relaxed ||
        config.sync_permissions !=
            onedrive::config::SyncPermissionsMode::umask ||
        config.local_conflict !=
            onedrive::config::LocalConflictPolicy::backup ||
        config.proxy.url !=
            std::optional<std::string>{
                "https://proxy.example.test:8443"
            } ||
        config.proxy.no_proxy !=
            std::optional<std::vector<std::string>>{
                {"localhost", ".internal.test"}
            } ||
        config.proxy.username !=
            std::optional<std::string>{"proxy-user"} ||
        config.proxy.password_file !=
            std::optional<std::filesystem::path>{
                path.parent_path() / "secrets/proxy-password"
            } ||
        config.proxy.auth != onedrive::http::ProxyAuth::digest ||
        config.proxy.ca_file !=
            std::optional<std::filesystem::path>{
                path.parent_path() / "certificates/proxy-ca.pem"
            } ||
        graph_options.download_transport.transfer !=
            config.transfer_transport ||
        graph_options.download_transport.
                maximum_receive_speed_bytes_per_second !=
            config.download_maximum_rate_bytes_per_second ||
        graph_options.download_maximum_retries != 3 ||
        graph_options.download_checkpoint_interval_bytes != 1024 ||
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
        onedrive::config::has_broad_auth_scope(defaults.auth_scope) ||
        !onedrive::config::has_broad_auth_scope(
            "User.Read Sites.ReadWrite.All offline_access"
        ) ||
        onedrive::config::has_broad_auth_scope(
            "User.Read Sites.ReadWrite.AllExtra offline_access"
        ) ||
        defaults.sync_permissions !=
            onedrive::config::SyncPermissionsMode::private_access ||
        defaults.local_conflict !=
            onedrive::config::LocalConflictPolicy::block ||
        defaults.sync_root_files ||
        defaults.transfer_order !=
            onedrive::config::TransferOrder::default_order ||
        defaults.proxy.url ||
        defaults.proxy.no_proxy ||
        defaults.proxy.username ||
        defaults.proxy.password_file ||
        defaults.proxy.auth !=
            onedrive::http::ProxyAuth::automatic ||
        defaults.proxy.ca_file ||
        defaults.download_maximum_retries != 4 ||
        defaults.transfer_transport.ip_version !=
            onedrive::http::IpVersion::automatic ||
        defaults.download_checkpoint_interval_bytes !=
            std::uint64_t{1024} * 1024U ||
        onedrive::app::graph_options(defaults).
            relaxed_download_validation ||
        !onedrive::app::graph_options(defaults).
            private_download_permissions) {
        std::cerr << "secure synchronization defaults were not applied\n";
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n";
    }
    try {
        const auto socks_proxy = onedrive::config::Config::load(path);
        if (socks_proxy.proxy.url !=
            std::optional<std::string>{
                "socks5h://127.0.0.1:1080"
            }) {
            std::filesystem::remove(path);
            std::cerr << "SOCKS5H proxy URL was not parsed correctly\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "SOCKS5H proxy URL was rejected: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    constexpr std::array proxy_auth_modes{
        std::pair{"auto", onedrive::http::ProxyAuth::automatic},
        std::pair{"basic", onedrive::http::ProxyAuth::basic},
        std::pair{"digest", onedrive::http::ProxyAuth::digest},
        std::pair{"ntlm", onedrive::http::ProxyAuth::ntlm},
        std::pair{"negotiate", onedrive::http::ProxyAuth::negotiate},
    };
    for (const auto& [name, expected] : proxy_auth_modes) {
        {
            std::ofstream output{path};
            output << "config_version = 2\n"
                   << "[proxy]\n"
                   << "url = \"https://proxy.example.test\"\n"
                   << "auth = \"" << name << "\"\n";
        }
        try {
            const auto proxy_auth =
                onedrive::config::Config::load(path);
            if (proxy_auth.proxy.auth != expected) {
                std::filesystem::remove(path);
                std::cerr << "proxy authentication mode was parsed "
                             "incorrectly\n";
                return EXIT_FAILURE;
            }
        } catch (const std::runtime_error& error) {
            std::filesystem::remove(path);
            std::cerr << "valid proxy authentication mode was rejected: "
                      << error.what() << '\n';
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "no_proxy = []\n";
    }
    try {
        const auto empty_no_proxy =
            onedrive::config::Config::load(path);
        if (!empty_no_proxy.proxy.no_proxy ||
            !empty_no_proxy.proxy.no_proxy->empty()) {
            std::filesystem::remove(path);
            std::cerr << "empty proxy bypass list was not preserved\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "empty proxy bypass list was rejected: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "no_proxy = [\"localhost,example.test\"]\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "ambiguous proxy bypass entry was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.no_proxy")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy bypass entry reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "auth = \"oauth\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsupported proxy authentication mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.auth")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy auth mode reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n"
               << "password_file = \"proxy-password\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "proxy password without username was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.username")) {
            std::filesystem::remove(path);
            std::cerr << "missing proxy username reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n"
               << "ca_file = \"proxy-ca.pem\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "proxy CA file with non-HTTPS URL was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.ca_file")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy CA file reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test:8443\"\n";
    }
    try {
        const auto https_proxy = onedrive::config::Config::load(path);
        if (https_proxy.proxy.url !=
            std::optional<std::string>{
                "https://proxy.example.test:8443"
            }) {
            std::filesystem::remove(path);
            std::cerr << "HTTPS proxy URL was not parsed correctly\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "HTTPS proxy URL was rejected: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"ftp://proxy.example.test:21\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsupported proxy URL was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.url")) {
            std::filesystem::remove(path);
            std::cerr << "unsupported proxy URL reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "local_conflict = \"overwrite\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsafe local conflict mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "sync_root_files = \"yes\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "non-boolean root-file policy was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "validation = \"unsafe\"\n";
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
        output << "config_version = 2\n"
               << "[sync]\n"
               << "sync_list = \"\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "empty selective sync path was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "ip_version = \"5\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download IP version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "transfer.ip_version"
            )) {
            std::filesystem::remove(path);
            std::cerr << "invalid download IP version reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "order = \"fastest\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid transfer order was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "transfer.order"
            )) {
            std::filesystem::remove(path);
            std::cerr << "invalid transfer order reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "maximum_retries = -1\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "negative download retry count was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("non-negative integer")) {
            std::filesystem::remove(path);
            std::cerr << "negative download retry count reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "checkpoint_interval_bytes = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download checkpoint interval was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "http_version = \"3\"\n";
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
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "operation_timeout_seconds = 0\n";
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
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "stall_minimum_bytes_per_second = 0\n";
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
        output << "config_version = 2\n"
               << "[download]\n"
               << "chunk_threshold_bytes = 0\n";
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
        output << "config_version = 2\n"
               << "[download]\n"
               << "concurrency = 17\n";
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
        output << "config_version = 2\n"
               << "[download]\n"
               << "concurrency = 0\n";
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
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
               << "[sync]\n"
               << "download_concurrency = 4\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "legacy version 1 download key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "unknown TOML configuration key 'sync.download_concurrency'"
            )) {
            std::filesystem::remove(path);
            std::cerr << "legacy download key reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown transfer key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown download key was accepted\n";
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
        output << "config_version = 1\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "version 1 TOML configuration was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "requires config_version = 2"
            )) {
            std::filesystem::remove(path);
            std::cerr << "version 1 configuration reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
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
        output << "config_version = 2\n"
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
