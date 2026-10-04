#pragma once

#include "onedrive/http/http_options.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace onedrive::config {

enum class FilesystemMetadataMode {
    automatic,
    xattr,
    database,
};

enum class DownloadValidationMode {
    strict,
    relaxed,
};

enum class SyncPermissionsMode {
    private_access,
    umask,
};

struct Config {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string drive_id{"me"};
    std::string application_id;
    std::string azure_tenant_id{"common"};
    std::string auth_endpoint{"https://login.microsoftonline.com"};
    std::string auth_scope{
        "User.Read Files.ReadWrite offline_access"
    };
    std::string graph_endpoint{"https://graph.microsoft.com/v1.0"};
    std::size_t graph_maximum_throttle_retries{4};
    std::chrono::seconds graph_initial_throttle_delay{1};
    std::chrono::seconds graph_maximum_throttle_delay{300};
    std::size_t download_concurrency{4};
    std::size_t download_maximum_retries{4};
    std::uint64_t download_chunk_threshold_bytes{
        std::uint64_t{8} * 1024U * 1024U
    };
    std::uint64_t download_checkpoint_interval_bytes{
        std::uint64_t{1024} * 1024U
    };
    http::DownloadTransportOptions download_transport;
    DownloadValidationMode download_validation{
        DownloadValidationMode::strict
    };
    SyncPermissionsMode sync_permissions{
        SyncPermissionsMode::private_access
    };
    FilesystemMetadataMode filesystem_metadata{
        FilesystemMetadataMode::automatic
    };
    bool dry_run{false};

    [[nodiscard]] static Config defaults();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
};

[[nodiscard]] bool has_auth_scope(
    std::string_view scopes,
    std::string_view expected
);
[[nodiscard]] bool has_broad_auth_scope(std::string_view scopes);

}  // namespace onedrive::config
