#include "checks.hpp"
#include "auth/options.hpp"
#include "onedrive/app/options.hpp"

#include "onedrive/util/mount.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::app::detail {

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

void secure_state_directory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::runtime_error("state.directory must not be empty");
    }
    onedrive::util::reject_symlink_components(
        directory,
        "state directory"
    );
    const bool created = std::filesystem::create_directories(directory);
    onedrive::util::reject_symlink_components(
        directory,
        "state directory"
    );

    onedrive::util::UniqueFD descriptor{
        ::open(
            directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open state directory '" + directory.string() + "': " +
            onedrive::util::system_error_message(errno)
        );
    }

    static_cast<void>(onedrive::util::secure_owned_directory(
        descriptor.get(),
        directory,
        private_directory_mode,
        "state directory"
    ));
    if (const auto error = descriptor.close(); error) {
        throw std::runtime_error(
            "cannot close state directory '" + directory.string() + "': " +
            error.message()
        );
    }
    if (created) {
        spdlog::debug(
            "Created private state directory '{}'",
            directory.string()
        );
    }
}

void secure_sync_directory(const std::filesystem::path& directory) {
    onedrive::util::UniqueFD descriptor{
        ::open(
            directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open sync directory '" + directory.string() + "': " +
            onedrive::util::system_error_message(errno)
        );
    }
    static_cast<void>(onedrive::util::secure_owned_directory(
        descriptor.get(),
        directory,
        private_directory_mode,
        "sync directory"
    ));
    if (const auto error = descriptor.close(); error) {
        throw std::runtime_error(
            "cannot close sync directory '" + directory.string() + "': " +
            error.message()
        );
    }
}

void validate_private_file(
    const std::filesystem::path& path,
    std::string_view description,
    bool required
) {
    std::error_code error;
    const bool exists = std::filesystem::exists(
        std::filesystem::symlink_status(path, error)
    );
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect " + std::string{description} + " '" +
            path.string() + "': " + error.message()
        );
    }
    if (!exists) {
        if (required) {
            throw std::runtime_error(
                std::string{description} +
                " is missing; run 'onedrive-cpp account login'"
            );
        }
        return;
    }

    onedrive::util::UniqueFD descriptor{
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open " + std::string{description} + " '" +
            path.string() + "': " + onedrive::util::system_error_message(errno)
        );
    }
    const auto status = onedrive::util::secure_owned_regular_file(
        descriptor.get(), path, private_file_mode, description
    );
    if (required && status.st_size == 0) {
        throw std::runtime_error(
            std::string{description} +
            " is empty; run 'onedrive-cpp account login'"
        );
    }
    if (const auto close_error = descriptor.close(); close_error) {
        throw std::runtime_error(
            "cannot close " + std::string{description} + " '" +
            path.string() + "': " + close_error.message()
        );
    }
}

void probe_writable_directory(const std::filesystem::path& directory) {
    const auto probe = directory /
        std::format(
            ".onedrive-preflight-{}-{}",
            ::getpid(),
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    onedrive::util::UniqueFD descriptor{
        ::open(
            probe.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            private_file_mode
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "sync directory is not writable '" + directory.string() + "': " +
            onedrive::util::system_error_message(errno)
        );
    }

    auto cleanup = [&probe] {
        std::error_code ignored;
        std::filesystem::remove(probe, ignored);
    };
    constexpr char content = '\0';
    if (::write(descriptor.get(), &content, 1) != 1 ||
        ::fsync(descriptor.get()) == -1) {
        const std::string message = onedrive::util::system_error_message(errno);
        descriptor.reset();
        cleanup();
        throw std::runtime_error(
            "cannot write and flush sync directory probe '" + probe.string() +
            "': " + message
        );
    }
    if (const auto error = descriptor.close(); error) {
        cleanup();
        throw std::runtime_error(
            "cannot close sync directory probe '" + probe.string() + "': " +
            error.message()
        );
    }
    if (!std::filesystem::remove(probe)) {
        throw std::runtime_error(
            "cannot remove sync directory probe '" + probe.string() + "'"
        );
    }

    onedrive::util::UniqueFD directory_descriptor{
        ::open(
            directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        )
    };
    if (!directory_descriptor) {
        throw std::runtime_error(
            "cannot open sync directory '" + directory.string() + "': " +
            onedrive::util::system_error_message(errno)
        );
    }
    if (::fsync(directory_descriptor.get()) == -1) {
        const std::string message = onedrive::util::system_error_message(errno);
        throw std::runtime_error(
            "cannot flush sync directory '" + directory.string() + "': " +
            message
        );
    }
    if (const auto error = directory_descriptor.close(); error) {
        throw std::runtime_error(
            "cannot close sync directory '" + directory.string() + "': " +
            error.message()
        );
    }
}

void prepare_sync_directory(
    const config::Config& config,
    Operation operation
) {
    if (config.sync_data_directory.empty()) {
        throw std::runtime_error("sync.data_directory must not be empty");
    }
    config::validate_sync_state_directories(
        config.sync_data_directory,
        config.state_directory
    );
    onedrive::util::reject_symlink_components(
        config.sync_data_directory,
        "sync directory"
    );
    onedrive::util::require_sync_mount(
        config.sync_data_directory,
        config.sync_data_mount_point
    );

    std::error_code error;
    const bool exists = std::filesystem::exists(
        std::filesystem::symlink_status(config.sync_data_directory, error)
    );
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect sync directory '" +
            config.sync_data_directory.string() + "': " + error.message()
        );
    }
    if (exists && !std::filesystem::is_directory(config.sync_data_directory)) {
        throw std::runtime_error(
            "sync.data_directory is not a directory: " +
            config.sync_data_directory.string()
        );
    }
    if (operation == Operation::monitor && !exists) {
        throw std::runtime_error(
            "monitor requires an existing sync.data_directory: " +
            config.sync_data_directory.string()
        );
    }
    if (operation != Operation::synchronize || config.dry_run) {
        return;
    }

    std::filesystem::create_directories(config.sync_data_directory);
    onedrive::util::reject_symlink_components(
        config.sync_data_directory,
        "sync directory"
    );
    if (config.sync_permissions ==
        config::SyncPermissionsMode::private_access) {
        secure_sync_directory(config.sync_data_directory);
    }
    probe_writable_directory(config.sync_data_directory);
}

void validate_authentication_config(const config::Config& config) {
    if (!auth::detail::valid_options(device_auth_options(config)) ||
        !config::has_auth_scope(config.auth_scope, "User.Read")) {
        throw std::runtime_error(
            "authentication requires auth.application_id, auth.tenant_id, an "
            "HTTPS auth.endpoint, and User.Read and offline_access scopes"
        );
    }
}

}  // namespace onedrive::app::detail
