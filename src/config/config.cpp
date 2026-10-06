#include "onedrive/config/config.hpp"
#include "config/parse.hpp"

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::config {

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .sync_directory = std::filesystem::path{home} / "OneDrive",
        .sync_mount_point = std::nullopt,
        .state_directory =
            std::filesystem::path{home} / ".local/state/onedrive-cpp",
        .sync_list = std::nullopt,
        .sync_root_files = false,
        .drive_id = "me",
        .application_id = {},
        .azure_tenant_id = "common",
        .auth_endpoint = "https://login.microsoftonline.com",
        .auth_scope = "User.Read Files.ReadWrite offline_access",
        .graph_endpoint = "https://graph.microsoft.com/v1.0",
        .graph_maximum_throttle_retries = 4,
        .graph_initial_throttle_delay = std::chrono::seconds{1},
        .graph_maximum_throttle_delay = std::chrono::seconds{300},
        .monitor_poll_interval = std::chrono::seconds{300},
        .monitor_settle_delay = std::chrono::milliseconds{1000},
        .download_concurrency = 4,
        .download_maximum_retries = 4,
        .download_chunk_threshold_bytes = std::uint64_t{8} * 1024U * 1024U,
        .download_checkpoint_interval_bytes = std::uint64_t{1024} * 1024U,
        .transfer_order = TransferOrder::default_order,
        .proxy = {},
        .transfer_transport = {},
        .download_maximum_rate_bytes_per_second = 0,
        .download_maximum_total_rate_bytes_per_second = 0,
        .upload_chunk_size_bytes = std::uint64_t{10} * 1024U * 1024U,
        .upload_concurrency = 1,
        .upload_maximum_rate_bytes_per_second = 0,
        .upload_maximum_total_rate_bytes_per_second = 0,
        .download_validation = DownloadValidationMode::strict,
        .sync_permissions = SyncPermissionsMode::private_access,
        .local_conflict = LocalConflictPolicy::block,
        .maximum_remote_deletions = 1000,
        .filesystem_metadata = FilesystemMetadataMode::automatic,
        .upload = true,
        .dry_run = false,
        .force_large_delete = false,
    };
}

Config Config::load(const std::filesystem::path& path) {
    Config config = defaults();
    if (!std::filesystem::exists(path)) {
        spdlog::debug("Configuration file not found; using default values");
        return config;
    }

    spdlog::debug("Loading configuration file");
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "cannot parse TOML configuration '" + path.string() +
            "': " + std::string{error.description()}
        );
    }

    detail::validate_keys(
        root,
        {
            "config_version",
            "sync",
            "proxy",
            "transfer",
            "download",
            "upload",
            "monitor",
            "state",
            "auth",
            "graph",
            "filesystem",
        },
        ""
    );
    const auto config_version = detail::optional_value<std::int64_t>(
        root,
        "config_version",
        "config_version",
        "an integer"
    );
    if (!config_version || *config_version != 2) {
        throw std::runtime_error(
            "TOML configuration requires config_version = 2"
        );
    }

    detail::load_sync_options(config, root, path);

    detail::load_proxy_options(config, root, path);

    detail::load_transfer_options(config, root);

    detail::load_download_options(config, root);

    detail::load_upload_options(config, root);

    detail::load_monitor_options(config, root);

    detail::load_state_options(config, root);

    detail::load_auth_options(config, root);

    detail::load_graph_options(config, root);

    detail::load_filesystem_options(config, root);

    return config;
}

bool has_auth_scope(
    std::string_view scopes,
    std::string_view expected
) {
    std::size_t position = 0;
    while (position < scopes.size()) {
        const auto start = scopes.find_first_not_of(" \t\r\n", position);
        if (start == std::string_view::npos) {
            return false;
        }
        const auto end = scopes.find_first_of(" \t\r\n", start);
        if (scopes.substr(start, end - start) == expected) {
            return true;
        }
        position = end == std::string_view::npos ? scopes.size() : end;
    }
    return false;
}

bool has_broad_auth_scope(std::string_view scopes) {
    return has_auth_scope(scopes, "Files.ReadWrite.All") ||
           has_auth_scope(scopes, "Sites.ReadWrite.All");
}

}  // namespace onedrive::config
