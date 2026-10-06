#pragma once

#include "onedrive/config/config.hpp"

#include <toml++/toml.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::config::detail {

void validate_keys(
    const toml::table& table,
    std::initializer_list<std::string_view> allowed,
    std::string_view table_name
);
[[nodiscard]] const toml::table* optional_table(
    const toml::table& parent,
    std::string_view key,
    std::string_view full_name
);

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

[[nodiscard]] std::uint64_t unsigned_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
);
[[nodiscard]] std::size_t size_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
);

template <typename Duration>
Duration duration_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto value = unsigned_value(table, key, full_name);
    using Representation = typename Duration::rep;
    if (value >
        static_cast<std::uint64_t>(
            std::numeric_limits<Representation>::max()
        )) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' is too large"
        );
    }
    return Duration{static_cast<Representation>(value)};
}

[[nodiscard]] std::vector<std::string> string_array_values(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
);
[[nodiscard]] std::string string_array_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
);
[[nodiscard]] FilesystemMetadataMode parse_filesystem_metadata(
    std::string_view value
);
[[nodiscard]] http::HttpVersion parse_http_version(std::string_view value);
[[nodiscard]] http::IpVersion parse_ip_version(std::string_view value);
void validate_proxy_url(std::string_view value);
[[nodiscard]] http::ProxyAuth parse_proxy_auth(std::string_view value);
[[nodiscard]] std::vector<std::string> proxy_bypass_list(
    const toml::table& table
);
[[nodiscard]] DownloadValidationMode parse_download_validation(
    std::string_view value
);
[[nodiscard]] TransferOrder parse_transfer_order(std::string_view value);
[[nodiscard]] SyncPermissionsMode parse_sync_permissions(
    std::string_view value
);
[[nodiscard]] LocalConflictPolicy parse_local_conflict(
    std::string_view value
);
[[nodiscard]] DotfilePolicy parse_dotfile_policy(std::string_view value);
[[nodiscard]] sync::SyncMode parse_sync_mode(std::string_view value);
[[nodiscard]] sync::DeletePolicy parse_delete_policy(std::string_view value);

void load_sync_options(
    Config& config,
    const toml::table& root,
    const std::filesystem::path& config_path
);
void load_output_options(
    Config& config,
    const toml::table& root,
    const std::filesystem::path& config_path
);
void load_proxy_options(
    Config& config,
    const toml::table& root,
    const std::filesystem::path& config_path
);
void load_transfer_options(Config& config, const toml::table& root);
void load_download_options(Config& config, const toml::table& root);
void load_upload_options(Config& config, const toml::table& root);
void load_monitor_options(Config& config, const toml::table& root);
void load_state_options(Config& config, const toml::table& root);
void load_auth_options(Config& config, const toml::table& root);
void load_graph_options(Config& config, const toml::table& root);
void load_filesystem_options(Config& config, const toml::table& root);

}  // namespace onedrive::config::detail
