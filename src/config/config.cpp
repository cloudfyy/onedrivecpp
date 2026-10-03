#include "onedrive/config/config.hpp"

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

std::chrono::seconds seconds_value(
    const toml::table& table,
    std::string_view key,
    std::string_view full_name
) {
    const auto value = unsigned_value(table, key, full_name);
    using SecondsRepresentation = std::chrono::seconds::rep;
    if (value >
        static_cast<std::uint64_t>(
            std::numeric_limits<SecondsRepresentation>::max()
        )) {
        throw std::runtime_error(
            "TOML configuration value '" + std::string{full_name} +
            "' is too large"
        );
    }
    return std::chrono::seconds{static_cast<SecondsRepresentation>(value)};
}

std::string string_array_value(
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

    std::string result;
    for (const auto& value : *values) {
        const auto string_value = value.value<std::string>();
        if (!string_value || string_value->empty()) {
            throw std::runtime_error(
                "TOML configuration value '" + std::string{full_name} +
                "' must contain only non-empty strings"
            );
        }
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += *string_value;
    }
    return result;
}

FilesystemMetadataMode parse_filesystem_metadata(std::string_view value) {
    if (value == "auto") {
        return FilesystemMetadataMode::automatic;
    }
    if (value == "xattr") {
        return FilesystemMetadataMode::xattr;
    }
    if (value == "database") {
        return FilesystemMetadataMode::database;
    }
    throw std::runtime_error(
        "invalid TOML configuration value for 'filesystem.metadata'"
    );
}

}  // namespace

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .sync_directory = std::filesystem::path{home} / "OneDrive",
        .state_directory = std::filesystem::path{home} / ".local/state/onedrive-cpp",
        .drive_id = "me",
        .application_id = {},
        .azure_tenant_id = "common",
        .auth_endpoint = "https://login.microsoftonline.com",
        .auth_scope =
            "User.Read Files.ReadWrite Files.ReadWrite.All Sites.ReadWrite.All "
            "offline_access",
        .graph_maximum_throttle_retries = 4,
        .graph_initial_throttle_delay = std::chrono::seconds{1},
        .graph_maximum_throttle_delay = std::chrono::seconds{300},
        .download_concurrency = 4,
        .download_chunk_threshold_bytes = 100U * 1024U * 1024U,
        .filesystem_metadata = FilesystemMetadataMode::automatic,
        .dry_run = false,
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
        {"config_version", "sync", "state", "auth", "graph", "filesystem"},
        ""
    );
    const auto config_version = optional_value<std::int64_t>(
        root,
        "config_version",
        "config_version",
        "an integer"
    );
    if (!config_version || *config_version != 1) {
        throw std::runtime_error(
            "TOML configuration requires config_version = 1"
        );
    }

    if (const auto* sync = optional_table(root, "sync", "sync")) {
        validate_keys(
            *sync,
            {
                "directory",
                "drive_id",
                "dry_run",
                "download_concurrency",
                "download_chunk_threshold_bytes",
            },
            "sync"
        );
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
                "dry_run",
                "sync.dry_run",
                "a boolean"
            )) {
            config.dry_run = *value;
        }
        if (sync->contains("download_concurrency")) {
            const auto concurrency = unsigned_value(
                *sync,
                "download_concurrency",
                "sync.download_concurrency"
            );
            if (concurrency < 1 || concurrency > 16) {
                throw std::runtime_error(
                    "sync.download_concurrency must be between 1 and 16"
                );
            }
            config.download_concurrency =
                static_cast<std::size_t>(concurrency);
        }
        if (sync->contains("download_chunk_threshold_bytes")) {
            const auto threshold = unsigned_value(
                *sync,
                "download_chunk_threshold_bytes",
                "sync.download_chunk_threshold_bytes"
            );
            if (threshold == 0) {
                throw std::runtime_error(
                    "sync.download_chunk_threshold_bytes must be greater than 0"
                );
            }
            config.download_chunk_threshold_bytes = threshold;
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
        validate_keys(*graph, {"throttle"}, "graph");
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
                const auto retries = unsigned_value(
                    *throttle,
                    "maximum_retries",
                    "graph.throttle.maximum_retries"
                );
                if (retries > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error(
                        "TOML configuration value "
                        "'graph.throttle.maximum_retries' is too large"
                    );
                }
                config.graph_maximum_throttle_retries =
                    static_cast<std::size_t>(retries);
            }
            if (throttle->contains("initial_delay_seconds")) {
                config.graph_initial_throttle_delay = seconds_value(
                    *throttle,
                    "initial_delay_seconds",
                    "graph.throttle.initial_delay_seconds"
                );
            }
            if (throttle->contains("maximum_delay_seconds")) {
                config.graph_maximum_throttle_delay = seconds_value(
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

}  // namespace onedrive::config
