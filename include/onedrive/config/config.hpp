#pragma once

#include "onedrive/http/http_options.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

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

struct Config {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string drive_id{"me"};
    std::string application_id;
    std::string azure_tenant_id{"common"};
    std::string auth_endpoint{"https://login.microsoftonline.com"};
    std::string auth_scope{
        "User.Read Files.ReadWrite Files.ReadWrite.All Sites.ReadWrite.All "
        "offline_access"
    };
    std::string graph_endpoint{"https://graph.microsoft.com/v1.0"};
    std::size_t graph_maximum_throttle_retries{4};
    std::chrono::seconds graph_initial_throttle_delay{1};
    std::chrono::seconds graph_maximum_throttle_delay{300};
    std::size_t download_concurrency{4};
    std::uint64_t download_chunk_threshold_bytes{
        std::uint64_t{8} * 1024U * 1024U
    };
    http::DownloadTransportOptions download_transport;
    DownloadValidationMode download_validation{
        DownloadValidationMode::strict
    };
    FilesystemMetadataMode filesystem_metadata{
        FilesystemMetadataMode::automatic
    };
    bool dry_run{false};

    [[nodiscard]] static Config defaults();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
};

}  // namespace onedrive::config
