#include "onedrive/config/config.hpp"

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
namespace {

void validate_keys(
    const toml::table& table,
    std::initializer_list<std::string_view> allowed,
    std::string_view table_name
) {
    for (const auto& [key, value] : table) {
        static_cast<void>(value);
        if (std::ranges::find(allowed, key.str()) == allowed.end()) {
            const std::string prefix =
                table_name.empty() ? "" : std::string{table_name} + ".";
            throw std::runtime_error(
                "unknown TOML configuration key '" + prefix +
                std::string{key.str()} + "'"
            );
        }
    }
}

const toml::table* optional_table(
    const toml::table& parent,
    std::string_view key,
    std::string_view full_name
) {
    const auto* node = parent.get(key);
    if (node == nullptr) {
        return nullptr;
    }
    const auto* table = node->as_table();
    if (table == nullptr) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' must be a table"
        );
    }
    return table;
}

template <typename Value>
std::optional<Value> optional_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name,
    std::string_view type_name
) {
    const auto* node = table.get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto value = node->value<Value>();
    if (!value) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' must be " + std::string{type_name}
        );
    }
    return value;
}

std::uint64_t unsigned_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto value =
        optional_value<std::int64_t>(table, key, full_name, "an integer");
    if (!value || *value < 0) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' must be a non-negative integer"
        );
    }
    return static_cast<std::uint64_t>(*value);
}

std::size_t size_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto value = unsigned_value(table, key, full_name);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' is too large"
        );
    }
    return static_cast<std::size_t>(value);
}

template <typename Duration>
Duration duration_value(
    const toml::table& table, std::string_view key, std::string_view full_name
) {
    const auto value = unsigned_value(table, key, full_name);
    using Representation = typename Duration::rep;
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<Representation>::max()
        )) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' is too large"
        );
    }
    return Duration{static_cast<Representation>(value)};
}

std::vector<std::string> string_array_values(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto* node = table.get(key);
    const auto* values = node == nullptr ? nullptr : node->as_array();
    if (values == nullptr) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' must be an array of strings"
        );
    }

    std::vector<std::string> result;
    result.reserve(values->size());
    for (const auto& value : *values) {
        const auto string_value = value.value<std::string>();
        if (!string_value || string_value->empty()) {
            throw std::runtime_error(
                "TOML configuration value '" + std::string{full_name} +
                "' must contain only non-empty strings"
            );
        }
        result.push_back(*string_value);
    }
    return result;
}

std::string string_array_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto values = string_array_values(table, key, full_name);
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += value;
    }
    return result;
}

template <typename Enum, typename Key, std::size_t Size>
Enum enum_value(
    std::string_view value,
    std::string_view full_name,
    const std::array<std::pair<Key, Enum>, Size>& values
) {
    for (const auto& [name, result] : values) {
        if (value == name) {
            return result;
        }
    }
    throw std::runtime_error(
        "invalid TOML configuration value for '" + std::string{full_name} + "'"
    );
}

FilesystemMetadataMode parse_filesystem_metadata(std::string_view value) {
    constexpr std::array values{
        std::pair{"auto", FilesystemMetadataMode::automatic},
        std::pair{"xattr", FilesystemMetadataMode::xattr},
        std::pair{"database", FilesystemMetadataMode::database},
    };
    return enum_value(value, "filesystem.metadata", values);
}

http::HttpVersion parse_http_version(std::string_view value) {
    constexpr std::array values{
        std::pair{"auto", http::HttpVersion::automatic},
        std::pair{"1.1", http::HttpVersion::http_1_1},
        std::pair{"2", http::HttpVersion::http_2},
    };
    return enum_value(value, "transfer.http_version", values);
}

http::IpVersion parse_ip_version(std::string_view value) {
    constexpr std::array values{
        std::pair{"auto", http::IpVersion::automatic},
        std::pair{"4", http::IpVersion::ipv4},
        std::pair{"6", http::IpVersion::ipv6},
    };
    return enum_value(value, "transfer.ip_version", values);
}

void validate_proxy_url(std::string_view value) {
    constexpr std::array schemes{
        "http://",
        "https://",
        "socks4://",
        "socks4a://",
        "socks5://",
        "socks5h://",
    };
    const auto scheme = std::ranges::find_if(
        schemes,
        [value](std::string_view candidate) {
            return value.starts_with(candidate);
        }
    );
    if (scheme == schemes.end() ||
        value.size() == std::string_view{*scheme}.size() ||
        value.contains('\0') ||
        std::ranges::any_of(value, [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        throw std::runtime_error(
            "invalid TOML configuration value for 'proxy.url'"
        );
    }
}

http::ProxyAuth parse_proxy_auth(std::string_view value) {
    constexpr std::array values{
        std::pair{"auto", http::ProxyAuth::automatic},
        std::pair{"basic", http::ProxyAuth::basic},
        std::pair{"digest", http::ProxyAuth::digest},
        std::pair{"ntlm", http::ProxyAuth::ntlm},
        std::pair{"negotiate", http::ProxyAuth::negotiate},
    };
    return enum_value(value, "proxy.auth", values);
}

std::vector<std::string> proxy_bypass_list(
    const toml::table& table
) {
    auto result =
        string_array_values(table, "no_proxy", "proxy.no_proxy");
    for (const auto& entry : result) {
        if (entry.contains('\0') || entry.contains(',') ||
            std::ranges::any_of(
                entry,
                [](unsigned char character) {
                    return std::isspace(character) != 0;
                }
            )) {
            throw std::runtime_error(
                "TOML configuration value 'proxy.no_proxy' must contain "
                "only non-empty entries without commas or whitespace"
            );
        }
    }
    return result;
}

DownloadValidationMode parse_download_validation(std::string_view value) {
    constexpr std::array values{
        std::pair{"strict", DownloadValidationMode::strict},
        std::pair{"relaxed", DownloadValidationMode::relaxed},
    };
    return enum_value(value, "download.validation", values);
}

TransferOrder parse_transfer_order(std::string_view value) {
    constexpr std::array values{
        std::pair{"default", TransferOrder::default_order},
        std::pair{"size_asc", TransferOrder::size_ascending},
        std::pair{"size_dsc", TransferOrder::size_descending},
        std::pair{"name_asc", TransferOrder::name_ascending},
        std::pair{"name_dsc", TransferOrder::name_descending},
    };
    return enum_value(value, "transfer.order", values);
}

SyncPermissionsMode parse_sync_permissions(std::string_view value) {
    constexpr std::array values{
        std::pair{"private", SyncPermissionsMode::private_access},
        std::pair{"umask", SyncPermissionsMode::umask},
    };
    return enum_value(value, "sync.permissions", values);
}

LocalConflictPolicy parse_local_conflict(std::string_view value) {
    constexpr std::array values{
        std::pair{"block", LocalConflictPolicy::block},
        std::pair{"backup", LocalConflictPolicy::backup},
    };
    return enum_value(value, "sync.local_conflict", values);
}

}  // namespace

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .sync_directory = std::filesystem::path{home} / "OneDrive",
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

    validate_keys(
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
    const auto config_version = optional_value<std::int64_t>(
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

    if (const auto* sync = optional_table(root, "sync", "sync")) {
        validate_keys(
            *sync,
            {
                "directory",
                "drive_id",
                "dry_run",
                "permissions",
                "sync_list",
                "sync_root_files",
                "local_conflict",
                "maximum_remote_deletions",
                "upload",
            },
            "sync"
        );
        if (const auto value = optional_value<std::string>(
                *sync,
                "local_conflict",
                "sync.local_conflict",
                "a string"
            )) {
            config.local_conflict = parse_local_conflict(*value);
        }
        if (sync->contains("maximum_remote_deletions")) {
            config.maximum_remote_deletions = size_value(
                *sync,
                "maximum_remote_deletions",
                "sync.maximum_remote_deletions"
            );
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "directory",
                "sync.directory",
                "a string"
            )) {
            config.sync_directory = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "drive_id",
                "sync.drive_id",
                "a string"
            )) {
            config.drive_id = *value;
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "upload",
                "sync.upload",
                "a boolean"
            )) {
            config.upload = *value;
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "dry_run",
                "sync.dry_run",
                "a boolean"
            )) {
            config.dry_run = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "sync_list",
                "sync.sync_list",
                "a string"
            )) {
            if (value->empty()) {
                throw std::runtime_error(
                    "sync.sync_list must not be empty"
                );
            }
            auto sync_list = std::filesystem::path{*value};
            if (sync_list.is_relative()) {
                sync_list = path.parent_path() / sync_list;
            }
            config.sync_list = sync_list.lexically_normal();
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "sync_root_files",
                "sync.sync_root_files",
                "a boolean"
            )) {
            config.sync_root_files = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "permissions",
                "sync.permissions",
                "a string"
            )) {
            config.sync_permissions = parse_sync_permissions(*value);
        }
    }

    if (const auto* proxy = optional_table(root, "proxy", "proxy")) {
        validate_keys(
            *proxy,
            {
                "url",
                "no_proxy",
                "username",
                "password_file",
                "auth",
                "ca_file",
            },
            "proxy"
        );
        const auto url = optional_value<std::string>(
            *proxy,
            "url",
            "proxy.url",
            "a string"
        );
        if (!url) {
            throw std::runtime_error(
                "TOML configuration value 'proxy.url' is required"
            );
        }
        validate_proxy_url(*url);
        config.proxy.url = *url;
        if (proxy->contains("no_proxy")) {
            config.proxy.no_proxy = proxy_bypass_list(*proxy);
        }
        if (const auto username = optional_value<std::string>(
                *proxy,
                "username",
                "proxy.username",
                "a string"
            )) {
            if (username->empty() || username->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.username' must not "
                    "be empty or contain NUL bytes"
                );
            }
            config.proxy.username = *username;
        }
        if (const auto password_file = optional_value<std::string>(
                *proxy,
                "password_file",
                "proxy.password_file",
                "a string"
            )) {
            if (password_file->empty() ||
                password_file->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.password_file' must "
                    "not be empty or contain NUL bytes"
                );
            }
            auto resolved = std::filesystem::path{*password_file};
            if (resolved.is_relative()) {
                resolved = path.parent_path() / resolved;
            }
            config.proxy.password_file = resolved.lexically_normal();
        }
        if (const auto auth = optional_value<std::string>(
                *proxy,
                "auth",
                "proxy.auth",
                "a string"
            )) {
            config.proxy.auth = parse_proxy_auth(*auth);
        }
        if (const auto ca_file = optional_value<std::string>(
                *proxy,
                "ca_file",
                "proxy.ca_file",
                "a string"
            )) {
            if (ca_file->empty() || ca_file->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.ca_file' must not "
                    "be empty or contain NUL bytes"
                );
            }
            if (!url->starts_with("https://")) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.ca_file' requires "
                    "an HTTPS proxy URL"
                );
            }
            auto resolved = std::filesystem::path{*ca_file};
            if (resolved.is_relative()) {
                resolved = path.parent_path() / resolved;
            }
            config.proxy.ca_file = resolved.lexically_normal();
        }
        if (config.proxy.password_file && !config.proxy.username) {
            throw std::runtime_error(
                "TOML configuration value 'proxy.password_file' requires "
                "'proxy.username'"
            );
        }
    }

    if (const auto* transfer =
            optional_table(root, "transfer", "transfer")) {
        auto& options = config.transfer_transport;
        validate_keys(
            *transfer,
            {
                "connect_timeout_seconds",
                "operation_timeout_seconds",
                "stall_timeout_seconds",
                "stall_minimum_bytes_per_second",
                "http_version",
                "ip_version",
                "order",
            },
            "transfer"
        );
        if (const auto value = optional_value<std::string>(
                *transfer,
                "order",
                "transfer.order",
                "a string"
            )) {
            config.transfer_order = parse_transfer_order(*value);
        }
        if (transfer->contains("connect_timeout_seconds")) {
            options.connect_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "connect_timeout_seconds",
                "transfer.connect_timeout_seconds"
            );
            if (options.connect_timeout == std::chrono::seconds::zero()) {
                throw std::runtime_error(
                    "transfer.connect_timeout_seconds must be greater than 0"
                );
            }
        }
        if (transfer->contains("operation_timeout_seconds")) {
            options.operation_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "operation_timeout_seconds",
                "transfer.operation_timeout_seconds"
            );
            if (options.operation_timeout == std::chrono::seconds::zero()) {
                throw std::runtime_error(
                    "transfer.operation_timeout_seconds must be greater than 0"
                );
            }
        }
        if (transfer->contains("stall_timeout_seconds")) {
            options.low_speed_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "stall_timeout_seconds",
                "transfer.stall_timeout_seconds"
            );
        }
        if (transfer->contains("stall_minimum_bytes_per_second")) {
            options.low_speed_limit_bytes_per_second = unsigned_value(
                *transfer,
                "stall_minimum_bytes_per_second",
                "transfer.stall_minimum_bytes_per_second"
            );
            if (options.low_speed_limit_bytes_per_second == 0) {
                throw std::runtime_error(
                    "transfer.stall_minimum_bytes_per_second must be greater "
                    "than 0"
                );
            }
        }
        if (const auto value = optional_value<std::string>(
                *transfer,
                "http_version",
                "transfer.http_version",
                "a string"
            )) {
            options.http_version = parse_http_version(*value);
        }
        if (const auto value = optional_value<std::string>(
                *transfer,
                "ip_version",
                "transfer.ip_version",
                "a string"
            )) {
            options.ip_version = parse_ip_version(*value);
        }
    }

    if (const auto* download =
            optional_table(root, "download", "download")) {
        validate_keys(
            *download,
            {
                "concurrency",
                "maximum_retries",
                "chunk_threshold_bytes",
                "checkpoint_interval_bytes",
                "maximum_rate_bytes_per_second",
                "maximum_total_rate_bytes_per_second",
                "validation",
            },
            "download"
        );
        if (download->contains("concurrency")) {
            const auto concurrency = unsigned_value(
                *download,
                "concurrency",
                "download.concurrency"
            );
            if (concurrency < 1 || concurrency > 16) {
                throw std::runtime_error(
                    "download.concurrency must be between 1 and 16"
                );
            }
            config.download_concurrency =
                static_cast<std::size_t>(concurrency);
        }
        if (download->contains("maximum_retries")) {
            config.download_maximum_retries = size_value(
                *download,
                "maximum_retries",
                "download.maximum_retries"
            );
        }
        if (download->contains("chunk_threshold_bytes")) {
            const auto threshold = unsigned_value(
                *download,
                "chunk_threshold_bytes",
                "download.chunk_threshold_bytes"
            );
            if (threshold == 0) {
                throw std::runtime_error(
                    "download.chunk_threshold_bytes must be greater than 0"
                );
            }
            config.download_chunk_threshold_bytes = threshold;
        }
        if (download->contains("checkpoint_interval_bytes")) {
            const auto interval = unsigned_value(
                *download,
                "checkpoint_interval_bytes",
                "download.checkpoint_interval_bytes"
            );
            if (interval == 0) {
                throw std::runtime_error(
                    "download.checkpoint_interval_bytes must be greater than 0"
                );
            }
            config.download_checkpoint_interval_bytes = interval;
        }
        if (download->contains("maximum_rate_bytes_per_second")) {
            config.download_maximum_rate_bytes_per_second =
                unsigned_value(
                    *download,
                    "maximum_rate_bytes_per_second",
                    "download.maximum_rate_bytes_per_second"
                );
        }
        if (download->contains(
                "maximum_total_rate_bytes_per_second"
            )) {
            config.download_maximum_total_rate_bytes_per_second =
                unsigned_value(
                    *download,
                    "maximum_total_rate_bytes_per_second",
                    "download.maximum_total_rate_bytes_per_second"
                );
        }
        if (const auto value = optional_value<std::string>(
                *download,
                "validation",
                "download.validation",
                "a string"
            )) {
            config.download_validation =
                parse_download_validation(*value);
        }
    }

    if (const auto* upload = optional_table(root, "upload", "upload")) {
        validate_keys(
            *upload,
            {
                "concurrency",
                "chunk_size_bytes",
                "maximum_rate_bytes_per_second",
                "maximum_total_rate_bytes_per_second",
            },
            "upload"
        );
        if (upload->contains("concurrency")) {
            const auto concurrency = unsigned_value(
                *upload,
                "concurrency",
                "upload.concurrency"
            );
            if (concurrency < 1 || concurrency > 16) {
                throw std::runtime_error(
                    "upload.concurrency must be between 1 and 16"
                );
            }
            config.upload_concurrency =
                static_cast<std::size_t>(concurrency);
        }
        if (upload->contains("chunk_size_bytes")) {
            const auto chunk_size = unsigned_value(
                *upload, "chunk_size_bytes", "upload.chunk_size_bytes"
            );
            constexpr std::uint64_t upload_quantum = std::uint64_t{320} * 1024U;
            constexpr std::uint64_t maximum_chunk_size =
                std::uint64_t{60} * 1024U * 1024U;
            if (chunk_size == 0 || chunk_size % upload_quantum != 0 ||
                chunk_size >= maximum_chunk_size) {
                throw std::runtime_error(
                    "upload.chunk_size_bytes must be a positive multiple of "
                    "320 KiB and less than 60 MiB"
                );
            }
            config.upload_chunk_size_bytes = chunk_size;
        }
        if (upload->contains("maximum_rate_bytes_per_second")) {
            config.upload_maximum_rate_bytes_per_second = unsigned_value(
                *upload,
                "maximum_rate_bytes_per_second",
                "upload.maximum_rate_bytes_per_second"
            );
        }
        if (upload->contains("maximum_total_rate_bytes_per_second")) {
            config.upload_maximum_total_rate_bytes_per_second = unsigned_value(
                *upload,
                "maximum_total_rate_bytes_per_second",
                "upload.maximum_total_rate_bytes_per_second"
            );
        }
    }

    if (const auto* monitor = optional_table(root, "monitor", "monitor")) {
        validate_keys(
            *monitor,
            {"poll_interval_seconds", "settle_delay_milliseconds"},
            "monitor"
        );
        if (monitor->contains("poll_interval_seconds")) {
            const auto interval = duration_value<std::chrono::seconds>(
                *monitor,
                "poll_interval_seconds",
                "monitor.poll_interval_seconds"
            );
            if (interval < std::chrono::seconds{1} ||
                interval > std::chrono::hours{24}) {
                throw std::runtime_error(
                    "monitor.poll_interval_seconds must be between 1 and 86400"
                );
            }
            config.monitor_poll_interval = interval;
        }
        if (monitor->contains("settle_delay_milliseconds")) {
            const auto delay = duration_value<std::chrono::milliseconds>(
                *monitor,
                "settle_delay_milliseconds",
                "monitor.settle_delay_milliseconds"
            );
            if (delay > std::chrono::minutes{1}) {
                throw std::runtime_error(
                    "monitor.settle_delay_milliseconds must not exceed 60000"
                );
            }
            config.monitor_settle_delay = delay;
        }
    }

    if (const auto* state = optional_table(root, "state", "state")) {
        validate_keys(*state, {"directory"}, "state");
        if (const auto value = optional_value<std::string>(
                *state,
                "directory",
                "state.directory",
                "a string"
            )) {
            config.state_directory = *value;
        }
    }

    if (const auto* auth = optional_table(root, "auth", "auth")) {
        validate_keys(
            *auth,
            {"application_id", "tenant_id", "endpoint", "scopes"},
            "auth"
        );
        if (const auto value = optional_value<std::string>(
                *auth,
                "application_id",
                "auth.application_id",
                "a string"
            )) {
            config.application_id = *value;
        }
        if (const auto value = optional_value<std::string>(
                *auth,
                "tenant_id",
                "auth.tenant_id",
                "a string"
            )) {
            config.azure_tenant_id = *value;
        }
        if (const auto value = optional_value<std::string>(
                *auth,
                "endpoint",
                "auth.endpoint",
                "a string"
            )) {
            config.auth_endpoint = *value;
        }
        if (auth->contains("scopes")) {
            config.auth_scope =
                string_array_value(*auth, "scopes", "auth.scopes");
        }
    }

    if (const auto* graph = optional_table(root, "graph", "graph")) {
        validate_keys(*graph, {"endpoint", "throttle"}, "graph");
        if (const auto value = optional_value<std::string>(
                *graph,
                "endpoint",
                "graph.endpoint",
                "a string"
            )) {
            config.graph_endpoint = *value;
        }
        if (const auto* throttle =
                optional_table(*graph, "throttle", "graph.throttle")) {
            validate_keys(
                *throttle,
                {
                    "maximum_retries",
                    "initial_delay_seconds",
                    "maximum_delay_seconds",
                },
                "graph.throttle"
            );
            if (throttle->contains("maximum_retries")) {
                config.graph_maximum_throttle_retries = size_value(
                    *throttle,
                    "maximum_retries",
                    "graph.throttle.maximum_retries"
                );
            }
            if (throttle->contains("initial_delay_seconds")) {
                config.graph_initial_throttle_delay =
                    duration_value<std::chrono::seconds>(
                        *throttle,
                        "initial_delay_seconds",
                        "graph.throttle.initial_delay_seconds"
                    );
            }
            if (throttle->contains("maximum_delay_seconds")) {
                config.graph_maximum_throttle_delay =
                    duration_value<std::chrono::seconds>(
                        *throttle,
                        "maximum_delay_seconds",
                        "graph.throttle.maximum_delay_seconds"
                    );
            }
        }
    }

    if (const auto* filesystem =
            optional_table(root, "filesystem", "filesystem")) {
        validate_keys(*filesystem, {"metadata"}, "filesystem");
        if (const auto value = optional_value<std::string>(
                *filesystem,
                "metadata",
                "filesystem.metadata",
                "a string"
            )) {
            config.filesystem_metadata = parse_filesystem_metadata(*value);
        }
    }

    if (config.graph_maximum_throttle_delay <
        config.graph_initial_throttle_delay) {
        throw std::runtime_error(
            "graph.throttle.maximum_delay_seconds must be greater than or equal "
            "to graph.throttle.initial_delay_seconds"
        );
    }
    spdlog::debug("TOML configuration loaded and validated");
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
