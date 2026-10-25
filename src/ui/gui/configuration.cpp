#include "configuration.hpp"
#include "onedrive/util/sha256.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/util/path_security.hpp"

#include "util/atomic_file.hpp"
#include "util/private_file.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace onedrive::gui {
namespace {

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr std::size_t maximum_config_bytes = std::size_t{1024} * 1024U;

void validate_gui_endpoints(const config::Config& config) {
    const auto validate =
        [](std::string_view name, const std::string& value, bool proxy) {
            if (proxy && value.empty()) {
                return;
            }
            const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url{
                curl_url(), curl_url_cleanup
            };
            if (!url) {
                throw std::runtime_error("cannot allocate endpoint URL parser");
            }
            const auto component = [&](CURLUPart part) {
                char* raw = nullptr;
                const auto result = curl_url_get(url.get(), part, &raw, 0);
                const std::unique_ptr<char, decltype(&curl_free)> owned{
                    raw, curl_free
                };
                return result == CURLUE_OK ? std::optional<std::string>{raw}
                                           : std::nullopt;
            };
            if (value.contains('\0') ||
                curl_url_set(
                    url.get(),
                    CURLUPART_URL,
                    value.c_str(),
                    CURLU_NON_SUPPORT_SCHEME
                ) != CURLUE_OK ||
                !component(CURLUPART_HOST) || component(CURLUPART_USER) ||
                component(CURLUPART_PASSWORD) || value.contains('?') ||
                value.contains('#') ||
                (!proxy && component(CURLUPART_SCHEME) != "https")) {
                throw std::invalid_argument(
                    std::string{name} +
                    ": expected a valid endpoint without inline credentials, "
                    "query or fragment" +
                    (proxy ? "" : " using HTTPS")
                );
            }
        };
    validate("auth.endpoint", config.auth_endpoint, false);
    validate("graph.endpoint", config.graph_endpoint, false);
    if (config.proxy.url) {
        validate("proxy.url", *config.proxy.url, true);
    }
}

std::string read_config_file(
    const std::filesystem::path& path, bool require_current_user_owner
) {
    const auto owner = require_current_user_owner
                           ? std::optional<uid_t>{::geteuid()}
                           : std::nullopt;
    return util::read_private_file(
        path,
        "configuration",
        {
            .required_owner = owner,
            .exact_permissions = std::nullopt,
            .forbidden_permissions = 0,
            .permission_requirement = "must be owned by the current user",
            .maximum_size = maximum_config_bytes,
            .require_single_link = true,
        }
    );
}

void ensure_private_parent(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    util::reject_symlink_components(parent, "configuration directory");
    std::filesystem::create_directories(parent);
    util::reject_symlink_components(parent, "configuration directory");
    util::secure_owned_directory(
        parent, private_directory_mode, "configuration directory"
    );
}

std::optional<std::filesystem::path>
backup_existing_config(const std::filesystem::path& config_file) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(config_file, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) {
        return std::nullopt;
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect configuration file '" + config_file.string() +
            "': " + error.message()
        );
    }

    const auto current = read_config_file(config_file, true);
    static std::atomic_uint64_t sequence{0};
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    auto backup = config_file;
    backup += std::format(
        ".backup.{}.{}",
        timestamp,
        sequence.fetch_add(1, std::memory_order_relaxed)
    );
    util::write_file_atomically(
        backup, current, private_file_mode, "configuration backup"
    );
    return backup;
}

std::string read_import_file(const std::filesystem::path& source_file) {
    return read_config_file(source_file, false);
}

} // namespace

config::Config load_configuration(
    const std::filesystem::path& config_file,
    bool& missing,
    std::string* fingerprint
) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(config_file, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) {
        missing = true;
        if (fingerprint) {
            fingerprint->clear();
        }
        return config::Config::defaults();
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect configuration file '" + config_file.string() +
            "': " + error.message()
        );
    }
    missing = false;
    try {
        const auto contents = read_config_file(config_file, false);
        auto config = config::Config::load_from_string(config_file, contents);
        if (fingerprint) {
            *fingerprint = util::sha256_hex(contents);
        }
        return config;
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(error.what());
    }
}

ConfigurationPreview preview_configuration(
    const std::filesystem::path& source_file,
    const std::filesystem::path& destination_file
) {
    const auto config = config::Config::load_from_string(
        destination_file, read_import_file(source_file)
    );
    validate_gui_endpoints(config);
    config::validate_sync_state_directories(
        config.sync_data_directory, config.state_directory
    );
    return {
        .sync_directory = config.sync_data_directory,
        .state_directory = config.state_directory,
        .sync_mode = config.sync_mode,
        .delete_policy = config.delete_policy,
    };
}

std::optional<std::filesystem::path> save_basic_settings(
    const std::filesystem::path& config_file,
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory
) {
    if (sync_directory.empty() || state_directory.empty()) {
        throw std::invalid_argument(
            "sync and state directories must not be empty"
        );
    }
    return save_settings_patch(
        config_file,
        {
            {"sync", "data_directory", sync_directory.string()},
            {"state", "directory", state_directory.string()},
        }
    );
}

std::optional<std::filesystem::path> save_settings_patch(
    const std::filesystem::path& config_file,
    const std::vector<config::SettingPatch>& patches
) {
    std::string current = "config_version = 2\n";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(config_file, error);
    if (!error && std::filesystem::exists(status)) {
        current = read_config_file(config_file, true);
    } else if (error != std::errc::no_such_file_or_directory && error) {
        throw std::runtime_error(
            "cannot inspect configuration file '" + config_file.string() +
            "': " + error.message()
        );
    }

    const auto updated = config::update_settings(config_file, current, patches);
    const auto updated_config =
        config::Config::load_from_string(config_file, updated);
    validate_gui_endpoints(updated_config);
    if (updated_config.sync_data_directory.empty() ||
        updated_config.state_directory.empty()) {
        throw std::invalid_argument(
            "sync and state directories must not be empty"
        );
    }
    config::validate_sync_state_directories(
        updated_config.sync_data_directory, updated_config.state_directory
    );
    ensure_private_parent(config_file);
    const auto backup = backup_existing_config(config_file);
    util::write_file_atomically(
        config_file, updated, private_file_mode, "configuration file"
    );
    return backup;
}

std::optional<std::filesystem::path> import_configuration(
    const std::filesystem::path& source_file,
    const std::filesystem::path& destination_file
) {
    const auto contents = read_import_file(source_file);
    const auto imported =
        config::Config::load_from_string(destination_file, contents);
    validate_gui_endpoints(imported);
    config::validate_sync_state_directories(
        imported.sync_data_directory, imported.state_directory
    );
    ensure_private_parent(destination_file);
    const auto backup = backup_existing_config(destination_file);
    util::write_file_atomically(
        destination_file, contents, private_file_mode, "configuration file"
    );
    return backup;
}

} // namespace onedrive::gui
