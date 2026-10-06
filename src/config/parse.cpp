#include "config/parse.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
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

sync::SyncMode parse_sync_mode(std::string_view value) {
    constexpr std::array values{
        std::pair{"bidirectional", sync::SyncMode::bidirectional},
        std::pair{"upload_only", sync::SyncMode::upload_only},
        std::pair{"download_only", sync::SyncMode::download_only},
    };
    return enum_value(value, "sync.mode", values);
}

sync::DeletePolicy parse_delete_policy(std::string_view value) {
    constexpr std::array values{
        std::pair{"propagate", sync::DeletePolicy::propagate},
        std::pair{"preserve", sync::DeletePolicy::preserve},
    };
    return enum_value(value, "sync.delete_policy", values);
}

}  // namespace onedrive::config::detail
