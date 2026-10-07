#include "support.hpp"

namespace {

using namespace onedrive::test::config;

int test_base() {
    ConfigFixture fixture;
    const auto& path = fixture.path;
    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[console]\n"
               << "color = \"always\"\n"
               << "ui = \"console\"\n"
               << "theme = \"synthwave\"\n"
               << "[logging]\n"
               << "level = \"debug\"\n"
               << "file = \"logs/onedrive-cpp.log\"\n"
               << "[sync]\n"
               << "data_directory = \"/tmp/OneDrive\"\n"
               << "data_mount_point = \"/tmp\"\n"
               << "drive_id = \"test-drive\"\n"
               << "sync_list = \"rules/sync_list\"\n"
               << "sync_root_files = true\n"
               << "nosync_enabled = false\n"
               << "dotfiles = \"exclude\"\n"
               << "maximum_file_size_bytes = 8388608\n"
               << "local_conflict = \"backup\"\n"
               << "maximum_remote_deletions = 42\n"
               << "mode = \"upload_only\"\n"
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
               << "maximum_total_rate_bytes_per_second = 2097152\n"
               << "validation = \"relaxed\"\n"
               << "[upload]\n"
               << "concurrency = 3\n"
               << "chunk_size_bytes = 10485760\n"
               << "maximum_rate_bytes_per_second = 3145728\n"
               << "maximum_total_rate_bytes_per_second = 4194304\n"
               << "[monitor]\n"
               << "poll_interval_seconds = 45\n"
               << "settle_delay_milliseconds = 250\n"
               << "websocket_enabled = false\n"
               << "websocket_request_timeout_seconds = 55\n"
               << "websocket_connect_timeout_seconds = 7\n"
               << "websocket_renewal_lead_seconds = 90\n"
               << "websocket_initial_backoff_seconds = 3\n"
               << "websocket_maximum_backoff_seconds = 30\n"
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

    if (config.console_color != onedrive::cli::ColorMode::always ||
        config.console_ui != onedrive::cli::UiMode::console ||
        config.console_theme != onedrive::cli::TuiTheme::synthwave ||
        config.logging.level != "debug" ||
        config.logging.file !=
            std::optional<std::filesystem::path>{
                path.parent_path() / "logs/onedrive-cpp.log"
            } ||
        config.sync_data_directory != "/tmp/OneDrive" ||
        config.sync_data_mount_point !=
            std::optional<std::filesystem::path>{"/tmp"} ||
        config.state_directory != "/tmp/onedrive-state" ||
        config.drive_id != "test-drive" ||
        config.sync_list !=
            std::optional<std::filesystem::path>{
                path.parent_path() / "rules/sync_list"
            } ||
        !config.sync_root_files ||
        config.nosync_enabled ||
        config.dotfiles != onedrive::config::DotfilePolicy::exclude ||
        config.maximum_file_size_bytes != 8'388'608 ||
        config.application_id != "test-application" ||
        config.azure_tenant_id != "test-tenant" ||
        config.auth_endpoint != "https://login.example.test" ||
        config.auth_scope != "Files.Read offline_access" ||
        config.graph_endpoint != "https://graph.example.test/v1.0" ||
        config.graph_maximum_throttle_retries != 7 ||
        config.graph_initial_throttle_delay != std::chrono::seconds{2} ||
        config.graph_maximum_throttle_delay != std::chrono::seconds{90} ||
        config.monitor_poll_interval != std::chrono::seconds{45} ||
        config.monitor_settle_delay != std::chrono::milliseconds{250} ||
        config.monitor_websocket_enabled ||
        config.monitor_websocket_request_timeout != std::chrono::seconds{55} ||
        config.monitor_websocket_connect_timeout != std::chrono::seconds{7} ||
        config.monitor_websocket_renewal_lead != std::chrono::seconds{90} ||
        config.monitor_websocket_initial_backoff != std::chrono::seconds{3} ||
        config.monitor_websocket_maximum_backoff != std::chrono::seconds{30} ||
        graph_options.notification_request_timeout !=
            std::chrono::seconds{55} ||
        config.transfer_order !=
            onedrive::config::TransferOrder::size_descending ||
        config.download_concurrency != 6 ||
        config.download_maximum_retries != 3 ||
        config.download_chunk_threshold_bytes != 4096 ||
        config.download_checkpoint_interval_bytes != 1024 ||
        config.transfer_transport.connect_timeout != std::chrono::seconds{12} ||
        config.transfer_transport.operation_timeout !=
            std::chrono::seconds{600} ||
        config.transfer_transport.low_speed_timeout !=
            std::chrono::seconds{15} ||
        config.transfer_transport.low_speed_limit_bytes_per_second != 128 ||
        config.download_maximum_rate_bytes_per_second != 1'048'576 ||
        config.download_maximum_total_rate_bytes_per_second != 2'097'152 ||
        config.upload_concurrency != 3 ||
        config.upload_chunk_size_bytes != 10'485'760 ||
        config.upload_maximum_rate_bytes_per_second != 3'145'728 ||
        config.upload_maximum_total_rate_bytes_per_second != 4'194'304 ||
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
        config.maximum_remote_deletions != 42 || config.force_large_delete ||
        config.sync_mode != onedrive::sync::SyncMode::upload_only ||
        config.delete_policy != onedrive::sync::DeletePolicy::preserve ||
        config.proxy.url !=
            std::optional<std::string>{"https://proxy.example.test:8443"} ||
        config.proxy.no_proxy !=
            std::optional<std::vector<std::string>>{
                {"localhost", ".internal.test"}
            } ||
        config.proxy.username != std::optional<std::string>{"proxy-user"} ||
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
        graph_options.download_transport
                .maximum_receive_speed_bytes_per_second !=
            config.download_maximum_rate_bytes_per_second ||
        graph_options.download_transport
                .maximum_total_receive_speed_bytes_per_second !=
            config.download_maximum_total_rate_bytes_per_second ||
        graph_options.upload_chunk_size_bytes !=
            config.upload_chunk_size_bytes ||
        graph_options.upload_transport.transfer != config.transfer_transport ||
        graph_options.upload_transport.maximum_send_speed_bytes_per_second !=
            config.upload_maximum_rate_bytes_per_second ||
        graph_options.upload_transport
                .maximum_total_send_speed_bytes_per_second !=
            config.upload_maximum_total_rate_bytes_per_second ||
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
        defaults.auth_scope != "User.Read Files.ReadWrite offline_access" ||
        !onedrive::config::has_auth_scope(
            defaults.auth_scope, "Files.ReadWrite"
        ) ||
        onedrive::config::has_auth_scope(
            defaults.auth_scope, "Files.ReadWrite.All"
        ) ||
        onedrive::config::has_broad_auth_scope(defaults.auth_scope) ||
        !onedrive::config::has_broad_auth_scope(
            "User.Read Sites.ReadWrite.All offline_access"
        ) ||
        !onedrive::config::has_broad_auth_scope(
            "User.Read Sites.Read.All offline_access"
        ) ||
        onedrive::config::has_broad_auth_scope(
            "User.Read Sites.ReadWrite.AllExtra offline_access"
        ) ||
        defaults.sync_permissions !=
            onedrive::config::SyncPermissionsMode::private_access ||
        defaults.sync_data_mount_point ||
        !defaults.nosync_enabled ||
        defaults.dotfiles != onedrive::config::DotfilePolicy::include ||
        defaults.maximum_file_size_bytes != 0 ||
        defaults.sync_mode != onedrive::sync::SyncMode::bidirectional ||
        defaults.delete_policy != onedrive::sync::DeletePolicy::propagate ||
        defaults.local_conflict !=
            onedrive::config::LocalConflictPolicy::block ||
        defaults.maximum_remote_deletions != 1000 ||
        defaults.force_large_delete ||
        defaults.monitor_poll_interval != std::chrono::seconds{300} ||
        defaults.monitor_settle_delay != std::chrono::milliseconds{1000} ||
        !defaults.monitor_websocket_enabled ||
        defaults.monitor_websocket_request_timeout !=
            std::chrono::seconds{60} ||
        defaults.monitor_websocket_connect_timeout !=
            std::chrono::seconds{10} ||
        defaults.monitor_websocket_renewal_lead != std::chrono::seconds{120} ||
        defaults.monitor_websocket_initial_backoff != std::chrono::seconds{1} ||
        defaults.monitor_websocket_maximum_backoff !=
            std::chrono::seconds{300} ||
        defaults.sync_root_files ||
        defaults.transfer_order !=
            onedrive::config::TransferOrder::default_order ||
        defaults.upload_concurrency != 1 ||
        defaults.upload_chunk_size_bytes != 10'485'760 ||
        defaults.upload_maximum_rate_bytes_per_second != 0 ||
        defaults.upload_maximum_total_rate_bytes_per_second != 0 ||
        defaults.proxy.url || defaults.proxy.no_proxy ||
        defaults.proxy.username || defaults.proxy.password_file ||
        defaults.proxy.auth != onedrive::http::ProxyAuth::automatic ||
        defaults.proxy.ca_file || defaults.download_maximum_retries != 4 ||
        defaults.download_maximum_total_rate_bytes_per_second != 0 ||
        defaults.transfer_transport.ip_version !=
            onedrive::http::IpVersion::automatic ||
        defaults.download_checkpoint_interval_bytes !=
            std::uint64_t{1024} * 1024U ||
        onedrive::app::graph_options(defaults).relaxed_download_validation ||
        !onedrive::app::graph_options(defaults).private_download_permissions) {
        std::cerr << "secure synchronization defaults were not applied\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_base();
}
