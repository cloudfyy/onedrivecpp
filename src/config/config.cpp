#include "onedrive/config/config.hpp"
#include "onedrive/util/path_security.hpp"
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::config {
namespace {

Config load_document(const std::filesystem::path& path, toml::table root) {
    Config config = Config::defaults();

    detail::validate_keys(
        root,
        {
            "config_version",
            "console",
            "logging",
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
        root, "config_version", "config_version", "an integer"
    );
    if (!config_version || *config_version != 2) {
        throw std::runtime_error(
            "TOML configuration requires config_version = 2"
        );
    }

    detail::load_sync_options(config, root, path);
    detail::load_output_options(config, root, path);
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

std::size_t
byte_offset(std::string_view contents, toml::source_position position) {
    std::size_t offset = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    while (offset < contents.size() &&
           (line < position.line || column < position.column)) {
        const auto byte = static_cast<unsigned char>(contents[offset]);
        if (byte == '\n') {
            ++line;
            column = 1;
            ++offset;
            continue;
        }
        if ((byte & 0xC0U) != 0x80U) {
            ++column;
        }
        ++offset;
    }
    if (line != position.line || column != position.column) {
        throw std::runtime_error(
            "cannot safely update the configuration source location"
        );
    }
    return offset;
}

std::string quoted_toml_string(std::string value) {
    std::ostringstream output;
    output << toml::toml_formatter{toml::value<std::string>{std::move(value)}};
    return output.str();
}

toml::node* find_path_value(
    toml::table& root, std::string_view table_name, std::string_view key
) {
    auto* table_node = root.get(table_name);
    auto* table = table_node == nullptr ? nullptr : table_node->as_table();
    return table == nullptr ? nullptr : table->get(key);
}

struct TextEdit {
    std::size_t begin;
    std::size_t end;
    std::string replacement;
};

TextEdit path_edit(
    std::string_view contents,
    toml::table& root,
    std::string_view table_name,
    std::string_view key,
    const std::filesystem::path& value
) {
    const auto formatted = quoted_toml_string(value.string());
    if (auto* node = find_path_value(root, table_name, key)) {
        if (!node->is_string()) {
            throw std::runtime_error(
                "configuration path '" + std::string{table_name} + "." +
                std::string{key} + "' must be a string"
            );
        }
        const auto source = node->source();
        return {
            byte_offset(contents, source.begin),
            byte_offset(contents, source.end),
            formatted,
        };
    }

    const auto header = "[" + std::string{table_name} + "]";
    std::size_t line_start = 0;
    while (line_start < contents.size()) {
        const auto newline = contents.find('\n', line_start);
        const auto line_end =
            newline == std::string::npos ? contents.size() : newline;
        auto line = std::string_view{contents}.substr(
            line_start, line_end - line_start
        );
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                                 line.front() == '\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line == header) {
            const auto insert_at =
                newline == std::string::npos ? contents.size() : newline + 1;
            const auto insertion = std::string{key} + " = " + formatted + "\n";
            return {insert_at, insert_at, insertion};
        }
        if (newline == std::string::npos) {
            break;
        }
        line_start = newline + 1;
    }

    std::string suffix;
    if (!contents.empty()) {
        suffix = contents.back() == '\n' ? "\n" : "\n\n";
    }
    suffix += "[" + std::string{table_name} + "]\n" + std::string{key} + " = " +
              formatted + "\n";
    return {contents.size(), contents.size(), suffix};
}

} // namespace

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .console_color = ColorMode::automatic,
        .console_ui = UiMode::automatic,
        .console_theme = TuiTheme::hacker,
        .logging = {},
        .sync_data_directory = std::filesystem::path{home} / "OneDrive",
        .sync_data_mount_point = std::nullopt,
        .state_directory =
            std::filesystem::path{home} / ".local/state/onedrive-cpp",
        .sync_list = std::nullopt,
        .sync_root_files = false,
        .nosync_enabled = true,
        .dotfiles = DotfilePolicy::include,
        .maximum_file_size_bytes = 0,
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
        .monitor_websocket_enabled = true,
        .monitor_websocket_request_timeout = std::chrono::seconds{60},
        .monitor_websocket_connect_timeout = std::chrono::seconds{10},
        .monitor_websocket_renewal_lead = std::chrono::seconds{120},
        .monitor_websocket_initial_backoff = std::chrono::seconds{1},
        .monitor_websocket_maximum_backoff = std::chrono::seconds{300},
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
        .sync_mode = sync::SyncMode::bidirectional,
        .delete_policy = sync::DeletePolicy::propagate,
        .maximum_remote_deletions = 1000,
        .filesystem_metadata = FilesystemMetadataMode::automatic,
        .dry_run = false,
        .force_large_delete = false,
    };
}

std::filesystem::path default_config_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path{home} / ".config/onedrive-cpp/config.toml";
    }
    return "/etc/onedrive-cpp/onedrive-cpp.toml";
}

Config Config::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        if (const auto logger = spdlog::default_logger()) {
            logger->debug("Configuration file not found; using default values");
        }
        return defaults();
    }

    if (const auto logger = spdlog::default_logger()) {
        logger->debug("Loading configuration file");
    }
    try {
        return load_document(path, toml::parse_file(path.string()));
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "cannot parse TOML configuration '" + path.string() +
            "': " + std::string{error.description()}
        );
    }
}

Config Config::load_from_string(
    const std::filesystem::path& path, std::string_view contents
) {
    try {
        return load_document(path, toml::parse(contents, path.string()));
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "cannot parse TOML configuration '" + path.string() +
            "': " + std::string{error.description()}
        );
    }
}

std::string update_basic_paths(
    const std::filesystem::path& config_path,
    std::string_view source,
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory
) {
    std::string contents{source};
    auto root = toml::parse(source, config_path.string());

    std::array edits{
        path_edit(contents, root, "sync", "data_directory", sync_directory),
        path_edit(contents, root, "state", "directory", state_directory),
    };
    std::ranges::sort(edits, [](const auto& left, const auto& right) {
        return left.begin > right.begin;
    });
    for (const auto& edit : edits) {
        contents.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    }
    static_cast<void>(Config::load_from_string(config_path, contents));
    return contents;
}

void validate_sync_state_directories(
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory
) {
    const auto sync = std::filesystem::weakly_canonical(
        util::normalized_absolute(sync_directory)
    );
    const auto state = std::filesystem::weakly_canonical(
        util::normalized_absolute(state_directory)
    );
    if (sync == sync.root_path()) {
        throw std::runtime_error(
            "sync.data_directory must not be the filesystem root"
        );
    }
    if (util::path_contains(sync, state) || util::path_contains(state, sync)) {
        throw std::runtime_error(
            "sync.data_directory and state.directory must not contain one "
            "another"
        );
    }
}

bool has_auth_scope(std::string_view scopes, std::string_view expected) {
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
           has_auth_scope(scopes, "Sites.Read.All") ||
           has_auth_scope(scopes, "Sites.ReadWrite.All");
}

} // namespace onedrive::config
