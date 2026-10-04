#include "runtime_preflight.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/path_security.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::app::detail {
namespace {

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

bool has_scope(std::string_view scopes, std::string_view expected) {
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

void secure_state_directory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::runtime_error("state.directory must not be empty");
    }
    onedrive::detail::reject_symlink_components(
        directory,
        "state directory"
    );
    const bool created = std::filesystem::create_directories(directory);
    onedrive::detail::reject_symlink_components(
        directory,
        "state directory"
    );

    const int descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open state directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }

    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot inspect state directory '" + directory.string() + "': " +
            message
        );
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()) {
        ::close(descriptor);
        throw std::runtime_error(
            "state directory must be owned by the current user: " +
            directory.string()
        );
    }
    if ((status.st_mode & 07777) != private_directory_mode &&
        ::fchmod(descriptor, private_directory_mode) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot secure state directory '" + directory.string() + "': " +
            message
        );
    }
    if (::close(descriptor) == -1) {
        throw std::runtime_error(
            "cannot close state directory '" + directory.string() + "': " +
            std::strerror(errno)
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
    const int descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open sync directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot inspect sync directory '" + directory.string() + "': " +
            message
        );
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()) {
        ::close(descriptor);
        throw std::runtime_error(
            "sync directory must be owned by the current user: " +
            directory.string()
        );
    }
    if ((status.st_mode & 07777) != private_directory_mode &&
        ::fchmod(descriptor, private_directory_mode) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot secure sync directory '" + directory.string() + "': " +
            message
        );
    }
    if (::close(descriptor) == -1) {
        throw std::runtime_error(
            "cannot close sync directory '" + directory.string() + "': " +
            std::strerror(errno)
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
                std::string{description} + " is missing; run 'onedrive-cpp auth'"
            );
        }
        return;
    }

    const int descriptor = ::open(
        path.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open " + std::string{description} + " '" +
            path.string() + "': " + std::strerror(errno)
        );
    }
    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot inspect " + std::string{description} + " '" +
            path.string() + "': " + message
        );
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid()) {
        ::close(descriptor);
        throw std::runtime_error(
            std::string{description} +
            " must be a regular file owned by the current user: " +
            path.string()
        );
    }
    if ((status.st_mode & 07777) != private_file_mode &&
        ::fchmod(descriptor, private_file_mode) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot secure " + std::string{description} + " '" +
            path.string() + "': " + message
        );
    }
    if (required && status.st_size == 0) {
        ::close(descriptor);
        throw std::runtime_error(
            std::string{description} + " is empty; run 'onedrive-cpp auth'"
        );
    }
    if (::close(descriptor) == -1) {
        throw std::runtime_error(
            "cannot close " + std::string{description} + " '" +
            path.string() + "': " + std::strerror(errno)
        );
    }
}

bool path_contains(
    const std::filesystem::path& parent,
    const std::filesystem::path& child
) {
    auto parent_part = parent.begin();
    auto child_part = child.begin();
    for (; parent_part != parent.end() && child_part != child.end();
         ++parent_part, ++child_part) {
        if (*parent_part != *child_part) {
            return false;
        }
    }
    return parent_part == parent.end();
}

void validate_distinct_directories(
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory
) {
    const auto sync = std::filesystem::weakly_canonical(
        onedrive::detail::normalized_absolute(sync_directory)
    );
    const auto state = std::filesystem::weakly_canonical(
        onedrive::detail::normalized_absolute(state_directory)
    );
    if (sync == sync.root_path()) {
        throw std::runtime_error(
            "sync.directory must not be the filesystem root"
        );
    }
    if (path_contains(sync, state) || path_contains(state, sync)) {
        throw std::runtime_error(
            "sync.directory and state.directory must not contain one another"
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
    const int descriptor = ::open(
        probe.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        private_file_mode
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "sync directory is not writable '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }

    auto cleanup = [&probe] {
        std::error_code ignored;
        std::filesystem::remove(probe, ignored);
    };
    constexpr char content = '\0';
    if (::write(descriptor, &content, 1) != 1 || ::fsync(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        cleanup();
        throw std::runtime_error(
            "cannot write and flush sync directory probe '" + probe.string() +
            "': " + message
        );
    }
    if (::close(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        cleanup();
        throw std::runtime_error(
            "cannot close sync directory probe '" + probe.string() + "': " +
            message
        );
    }
    if (!std::filesystem::remove(probe)) {
        throw std::runtime_error(
            "cannot remove sync directory probe '" + probe.string() + "'"
        );
    }

    const int directory_descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (directory_descriptor == -1) {
        throw std::runtime_error(
            "cannot open sync directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
    if (::fsync(directory_descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::close(directory_descriptor);
        throw std::runtime_error(
            "cannot flush sync directory '" + directory.string() + "': " +
            message
        );
    }
    if (::close(directory_descriptor) == -1) {
        throw std::runtime_error(
            "cannot close sync directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
}

void prepare_sync_directory(
    const config::Config& config,
    Operation operation
) {
    if (config.sync_directory.empty()) {
        throw std::runtime_error("sync.directory must not be empty");
    }
    validate_distinct_directories(
        config.sync_directory,
        config.state_directory
    );
    onedrive::detail::reject_symlink_components(
        config.sync_directory,
        "sync directory"
    );

    std::error_code error;
    const bool exists = std::filesystem::exists(
        std::filesystem::symlink_status(config.sync_directory, error)
    );
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect sync directory '" +
            config.sync_directory.string() + "': " + error.message()
        );
    }
    if (exists && !std::filesystem::is_directory(config.sync_directory)) {
        throw std::runtime_error(
            "sync.directory is not a directory: " +
            config.sync_directory.string()
        );
    }
    if (operation == Operation::monitor && !exists) {
        throw std::runtime_error(
            "monitor requires an existing sync.directory: " +
            config.sync_directory.string()
        );
    }
    if (operation != Operation::synchronize || config.dry_run) {
        return;
    }

    std::filesystem::create_directories(config.sync_directory);
    onedrive::detail::reject_symlink_components(
        config.sync_directory,
        "sync directory"
    );
    if (config.sync_permissions ==
        config::SyncPermissionsMode::private_access) {
        secure_sync_directory(config.sync_directory);
    }
    probe_writable_directory(config.sync_directory);
}

void validate_authentication_config(const config::Config& config) {
    if (config.application_id.empty() || config.azure_tenant_id.empty() ||
        config.auth_scope.empty() ||
    !has_scope(config.auth_scope, "User.Read") ||
    !has_scope(config.auth_scope, "offline_access") ||
        !config.auth_endpoint.starts_with("https://")) {
        throw std::runtime_error(
            "authentication requires auth.application_id, auth.tenant_id, an "
            "HTTPS auth.endpoint, and User.Read and offline_access scopes"
        );
    }
}

int acquire_runtime_lock(const std::filesystem::path& state_directory) {
    const auto path = state_directory / "onedrive-cpp.lock";
    const int descriptor = ::open(
        path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        private_file_mode
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open runtime lock '" + path.string() + "': " +
            std::strerror(errno)
        );
    }
    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot use runtime lock '" + path.string() + "': " + message
        );
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1) {
        ::close(descriptor);
        throw std::runtime_error(
            "cannot use runtime lock '" + path.string() +
            "': unsafe lock file"
        );
    }
    if (::fchmod(descriptor, private_file_mode) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot secure runtime lock '" + path.string() + "': " + message
        );
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) == -1) {
        const std::string message = errno == EWOULDBLOCK ?
            "another onedrive-cpp process is already using this state directory" :
            std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot acquire runtime lock '" + path.string() + "': " + message
        );
    }

    const std::string process_id = std::to_string(::getpid()) + "\n";
    if (::ftruncate(descriptor, 0) == -1 ||
        ::write(descriptor, process_id.data(), process_id.size()) !=
            static_cast<ssize_t>(process_id.size()) ||
        ::fsync(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::flock(descriptor, LOCK_UN);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot update runtime lock '" + path.string() + "': " + message
        );
    }
    return descriptor;
}

}  // namespace

RuntimePreflight::RuntimePreflight(
    const config::Config& config,
    Operation operation
) {
    if (operation == Operation::authenticate ||
        operation == Operation::synchronize) {
        validate_authentication_config(config);
    }
    if ((operation == Operation::reset_state ||
         operation == Operation::synchronize) &&
        config.drive_id.empty()) {
        throw std::runtime_error("sync.drive_id must not be empty");
    }

    secure_state_directory(config.state_directory);
    lock_descriptor_ = acquire_runtime_lock(config.state_directory);

    try {
        validate_private_file(
            config.state_directory / "active_account",
            "active account marker",
            false
        );
        const bool authentication_required =
            operation == Operation::reset_state ||
            operation == Operation::synchronize;
        const auto token_directory =
            account::AccountState::find_active_token_directory(
                config.state_directory
            );
        if (authentication_required && !token_directory) {
            throw std::runtime_error(
                "active Microsoft account is missing; run 'onedrive-cpp auth' "
                "to initialize the account-based state layout"
            );
        }
        if (token_directory) {
            validate_private_file(
                *token_directory / "refresh_token",
                "refresh token",
                authentication_required
            );
        }
        if (operation == Operation::synchronize ||
            operation == Operation::monitor) {
            prepare_sync_directory(config, operation);
        }
    } catch (...) {
        ::flock(lock_descriptor_, LOCK_UN);
        ::close(lock_descriptor_);
        lock_descriptor_ = -1;
        throw;
    }
}

RuntimePreflight::~RuntimePreflight() {
    if (lock_descriptor_ != -1) {
        ::flock(lock_descriptor_, LOCK_UN);
        ::close(lock_descriptor_);
    }
}

}  // namespace onedrive::app::detail
