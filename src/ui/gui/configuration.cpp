#include "configuration.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/util/path_security.hpp"

#include "util/atomic_file.hpp"
#include "util/private_file.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <optional>
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
constexpr std::size_t maximum_config_bytes = 1024U * 1024U;

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
    auto directory =
        util::open_path_no_symlinks(parent, O_RDONLY | O_DIRECTORY);
    static_cast<void>(util::secure_owned_directory(
        directory.get(),
        parent,
        private_directory_mode,
        "configuration directory"
    ));
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

std::string sync_mode_name(sync::SyncMode mode) {
    switch (mode) {
    case sync::SyncMode::bidirectional:
        return "Bidirectional";
    case sync::SyncMode::upload_only:
        return "Upload only";
    case sync::SyncMode::download_only:
        return "Download only";
    }
    throw std::logic_error{"unknown synchronization mode"};
}

std::string delete_policy_name(sync::DeletePolicy policy) {
    switch (policy) {
    case sync::DeletePolicy::propagate:
        return "Propagate deletions";
    case sync::DeletePolicy::preserve:
        return "Preserve destination files";
    }
    throw std::logic_error{"unknown deletion policy"};
}

} // namespace

ConfigurationPreview preview_configuration(
    const std::filesystem::path& source_file,
    const std::filesystem::path& destination_file
) {
    const auto config = config::Config::load_from_string(
        destination_file, read_import_file(source_file)
    );
    config::validate_sync_state_directories(
        config.sync_data_directory, config.state_directory
    );
    return {
        .sync_directory = config.sync_data_directory,
        .state_directory = config.state_directory,
        .sync_mode = sync_mode_name(config.sync_mode),
        .delete_policy = delete_policy_name(config.delete_policy),
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
    ensure_private_parent(config_file);

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

    const auto updated = config::update_basic_paths(
        config_file, current, sync_directory, state_directory
    );
    const auto updated_config =
        config::Config::load_from_string(config_file, updated);
    config::validate_sync_state_directories(
        updated_config.sync_data_directory, updated_config.state_directory
    );
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
