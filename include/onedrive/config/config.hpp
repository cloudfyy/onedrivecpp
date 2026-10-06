#pragma once

#include "onedrive/cli/console.hpp"
#include "onedrive/http/http_options.hpp"
#include "onedrive/logging/logging.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

enum class LocalConflictPolicy {
    block,
    backup,
};

enum class DotfilePolicy {
    include,
    exclude,
};

enum class TransferOrder {
    default_order,
    size_ascending,
    size_descending,
    name_ascending,
    name_descending,
};

struct Config {
    cli::ColorMode console_color{cli::ColorMode::automatic};
    logging::Options logging;
    std::filesystem::path sync_data_directory;
    std::optional<std::filesystem::path> sync_data_mount_point;
    std::filesystem::path state_directory;
    std::optional<std::filesystem::path> sync_list;
    bool sync_root_files{false};
    bool nosync_enabled{true};
    DotfilePolicy dotfiles{DotfilePolicy::include};
    std::uint64_t maximum_file_size_bytes{0};
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
    std::chrono::seconds monitor_poll_interval{300};
    std::chrono::milliseconds monitor_settle_delay{1000};
    bool monitor_websocket_enabled{true};
    std::chrono::seconds monitor_websocket_request_timeout{60};
    std::chrono::seconds monitor_websocket_connect_timeout{10};
    std::chrono::seconds monitor_websocket_renewal_lead{120};
    std::chrono::seconds monitor_websocket_initial_backoff{1};
    std::chrono::seconds monitor_websocket_maximum_backoff{300};
    std::size_t download_concurrency{4};
    std::size_t download_maximum_retries{4};
    std::uint64_t download_chunk_threshold_bytes{
        std::uint64_t{8} * 1024U * 1024U
    };
    std::uint64_t download_checkpoint_interval_bytes{
        std::uint64_t{1024} * 1024U
    };
    TransferOrder transfer_order{TransferOrder::default_order};
    http::ProxyOptions proxy;
    http::TransferTransportOptions transfer_transport;
    std::uint64_t download_maximum_rate_bytes_per_second{0};
    std::uint64_t download_maximum_total_rate_bytes_per_second{0};
    std::uint64_t upload_chunk_size_bytes{std::uint64_t{10} * 1024U * 1024U};
    std::size_t upload_concurrency{1};
    std::uint64_t upload_maximum_rate_bytes_per_second{0};
    std::uint64_t upload_maximum_total_rate_bytes_per_second{0};
    DownloadValidationMode download_validation{
        DownloadValidationMode::strict
    };
    SyncPermissionsMode sync_permissions{
        SyncPermissionsMode::private_access
    };
    LocalConflictPolicy local_conflict{LocalConflictPolicy::block};
    sync::SyncMode sync_mode{sync::SyncMode::bidirectional};
    sync::DeletePolicy delete_policy{sync::DeletePolicy::propagate};
    std::size_t maximum_remote_deletions{1000};
    FilesystemMetadataMode filesystem_metadata{
        FilesystemMetadataMode::automatic
    };
    bool dry_run{false};
    bool force_large_delete{false};

    [[nodiscard]] static Config defaults();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
};

[[nodiscard]] bool has_auth_scope(
    std::string_view scopes,
    std::string_view expected
);
[[nodiscard]] bool has_broad_auth_scope(std::string_view scopes);

}  // namespace onedrive::config
