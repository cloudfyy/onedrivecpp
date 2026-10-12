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
#include <type_traits>
#include <vector>

namespace onedrive::config {
namespace {

Config
load_document(const std::filesystem::path& path, const toml::table& root) {
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

std::string array_comments(std::string_view contents, const toml::node& node) {
    const auto* array = node.as_array();
    if (!array) {
        return {};
    }
    std::string comments;
    const auto collect = [&](std::size_t begin, std::size_t end) {
        const auto gap = contents.substr(begin, end - begin);
        std::size_t position = 0;
        while ((position = gap.find('#', position)) != std::string_view::npos) {
            const auto newline = gap.find('\n', position);
            comments += gap.substr(
                position,
                newline == std::string_view::npos ? gap.size() - position
                                                  : newline - position
            );
            comments += '\n';
            if (newline == std::string_view::npos) {
                break;
            }
            position = newline + 1;
        }
    };
    auto begin = byte_offset(contents, node.source().begin);
    for (const auto& entry : *array) {
        collect(begin, byte_offset(contents, entry.source().begin));
        begin = byte_offset(contents, entry.source().end);
    }
    collect(begin, byte_offset(contents, node.source().end));
    return comments;
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
    return update_settings(
        config_path,
        source,
        {
            {"sync", "data_directory", sync_directory.string()},
            {"state", "directory", state_directory.string()},
        }
    );
}

std::string update_settings(
    const std::filesystem::path& config_path,
    std::string_view source,
    const std::vector<SettingPatch>& patches
) {
    static_cast<void>(Config::load_from_string(config_path, source));
    std::string contents{source};
    for (const auto& patch : patches) {
        // Reparse after each edit: missing siblings may share an insertion
        // point.
        auto root = toml::parse(contents, config_path.string());
        auto* table = root.at_path(patch.table).as_table();
        if (patch.key.empty()) {
            if (!std::holds_alternative<std::monostate>(patch.value) ||
                patch.table.contains('.')) {
                throw std::invalid_argument(
                    "table removal requires a top-level table and no value"
                );
            }
            if (!table) {
                continue;
            }
            struct Removal {
                std::size_t begin;
                std::size_t end;
                std::string comments;
            };
            std::vector<Removal> removals;
            if (table->is_inline()) {
                const auto key = root.find(patch.table);
                std::string comments;
                for (const auto& [child_key, value] : *table) {
                    comments += array_comments(contents, value);
                }
                removals.push_back(
                    {byte_offset(contents, key->first.source().begin),
                     byte_offset(contents, table->source().end),
                     std::move(comments)}
                );
            } else {
                const auto header =
                    byte_offset(contents, table->source().begin);
                if (contents[header] == '[') {
                    removals.push_back(
                        {header, contents.find(']', header) + 1, {}}
                    );
                }
                for (const auto& [key, value] : *table) {
                    if (value.is_table()) {
                        throw std::invalid_argument(
                            "cannot remove a table containing child tables"
                        );
                    }
                    auto begin = byte_offset(contents, key.source().begin);
                    const auto newline = contents.rfind('\n', begin);
                    begin = newline == std::string::npos ? 0 : newline + 1;
                    removals.push_back(
                        {begin,
                         byte_offset(contents, value.source().end),
                         array_comments(contents, value)}
                    );
                }
            }
            std::sort(
                removals.begin(),
                removals.end(),
                [](const auto& a, const auto& b) { return a.begin > b.begin; }
            );
            for (const auto& [begin, end, comments] : removals) {
                contents.replace(begin, end - begin, comments);
            }
            continue;
        }
        auto* node = table ? table->get(patch.key) : nullptr;
        std::string formatted;
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                    toml::array array;
                    for (const auto& entry : value) {
                        array.push_back(entry);
                    }
                    std::ostringstream output;
                    output << toml::toml_formatter{array};
                    formatted = output.str();
                } else if constexpr (!std::is_same_v<T, std::monostate>) {
                    std::ostringstream output;
                    output << toml::toml_formatter{toml::value<T>{value}};
                    formatted = output.str();
                }
            },
            patch.value
        );
        if (node && !formatted.empty()) {
            const auto begin = byte_offset(contents, node->source().begin);
            const auto end = byte_offset(contents, node->source().end);
            const auto comments = array_comments(contents, *node);
            if (!comments.empty()) {
                formatted.insert(1, "\n" + comments);
            }
            contents.replace(begin, end - begin, formatted);
        } else if (table && table->is_inline()) {
            const auto begin = byte_offset(contents, table->source().begin);
            const auto end = byte_offset(contents, table->source().end);
            if (formatted.empty()) {
                if (node) {
                    const auto key = table->find(patch.key);
                    auto first =
                        byte_offset(contents, key->first.source().begin);
                    auto last = byte_offset(contents, node->source().end);
                    const auto next =
                        contents.find_first_not_of(" \t\r\n", last);
                    if (next < end && contents[next] == ',') {
                        last = next + 1;
                    } else {
                        const auto previous =
                            contents.find_last_not_of(" \t\r\n", first - 1);
                        if (previous > begin && contents[previous] == ',') {
                            first = previous;
                        }
                    }
                    const auto comments = array_comments(contents, *node);
                    contents.erase(first, last - first);
                    contents.insert(0, comments);
                }
            } else {
                contents.insert(
                    end - 1,
                    (table->empty() ? "" : ", ") + patch.key + " = " +
                        formatted + " "
                );
            }
        } else if (node) {
            const auto key = table->find(patch.key);
            auto begin = byte_offset(contents, key->first.source().begin);
            const auto end = byte_offset(contents, node->source().end);
            const auto newline = contents.rfind('\n', begin);
            begin = newline == std::string::npos ? 0 : newline + 1;
            contents.replace(
                begin, end - begin, array_comments(contents, *node)
            );
        } else if (!formatted.empty()) {
            if (!table) {
                const auto dot = patch.table.rfind('.');
                if (dot != std::string::npos) {
                    auto* parent =
                        root.at_path(patch.table.substr(0, dot)).as_table();
                    if (parent && parent->is_inline()) {
                        const auto end =
                            byte_offset(contents, parent->source().end);
                        contents.insert(
                            end - 1,
                            (parent->empty() ? "" : ", ") +
                                patch.table.substr(dot + 1) + " = { " +
                                patch.key + " = " + formatted + " } "
                        );
                        continue;
                    }
                }
                contents += "\n[" + patch.table + "]\n" + patch.key + " = " +
                            formatted + "\n";
            } else {
                const auto begin = byte_offset(contents, table->source().begin);
                if (contents[begin] == '[') {
                    const auto newline = contents.find('\n', begin);
                    const auto header = contents.substr(begin, newline - begin);
                    const auto probe = toml::parse(
                        header + "\n__setting_patch_probe__ = true\n"
                    );
                    const auto* header_table =
                        probe.at_path(patch.table).as_table();
                    if (!header_table ||
                        !header_table->contains("__setting_patch_probe__")) {
                        // An implicit parent of a declared child table can be
                        // explicitly declared at the end of the document.
                        contents += "\n[" + patch.table + "]\n" + patch.key +
                                    " = " + formatted + "\n";
                        continue;
                    }
                    const auto at = newline == std::string::npos
                                        ? contents.size()
                                        : newline + 1;
                    contents.insert(
                        at,
                        (newline == std::string::npos ? "\n" : "") + patch.key +
                            " = " + formatted + "\n"
                    );
                } else {
                    contents.insert(
                        0,
                        patch.table + "." + patch.key + " = " + formatted + "\n"
                    );
                }
            }
        }
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
