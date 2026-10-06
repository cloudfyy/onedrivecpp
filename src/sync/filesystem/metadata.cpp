#include "sync/filesystem/metadata.hpp"

#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/filesystem/operations.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/xattr.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

bool probe_xattr_support(const std::filesystem::path& root) {
    const auto probe = root / std::format(
                                  ".onedrive-cpp-xattr-probe-{}",
                                  ::getpid()
                              );
    onedrive::util::UniqueFD descriptor{
        ::open(
            probe.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot create filesystem capability probe in '" + root.string() +
            "': " + std::strerror(errno)
        );
    }
    if (const auto close_error = descriptor.close(); close_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(probe, cleanup_error);
        if (cleanup_error) {
            spdlog::warn(
                "Could not remove filesystem capability probe '{}': {}",
                probe.string(),
                cleanup_error.message()
            );
        }
        throw std::runtime_error(
            "cannot close filesystem capability probe: " +
            close_error.message()
        );
    }

    constexpr std::string_view expected{"supported"};
    if (::setxattr(
            probe.c_str(),
            "user.onedrive.probe",
            expected.data(),
            expected.size(),
            0
        ) == -1) {
        const int error = errno;
        std::error_code cleanup_error;
        std::filesystem::remove(probe, cleanup_error);
        if (cleanup_error) {
            spdlog::warn(
                "Could not remove filesystem capability probe '{}': {}",
                probe.string(),
                cleanup_error.message()
            );
        }
        if (xattr_error_is_unavailable(error)) {
            spdlog::debug(
                "User xattr probe is unavailable for '{}': {}",
                root.string(),
                std::strerror(error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot probe user extended attributes in '" + root.string() +
            "': " + std::strerror(error)
        );
    }

    std::array<char, 32> value{};
    const auto size = ::getxattr(
        probe.c_str(),
        "user.onedrive.probe",
        value.data(),
        value.size()
    );
    const int read_error = size == -1 ? errno : 0;
    const bool valid =
        size == static_cast<ssize_t>(expected.size()) &&
        std::string_view{value.data(), static_cast<std::size_t>(size)} == expected;
    const int remove_result =
        ::removexattr(probe.c_str(), "user.onedrive.probe");
    const int remove_error = remove_result == -1 ? errno : 0;
    std::error_code cleanup_error;
    const bool probe_removed = std::filesystem::remove(probe, cleanup_error);
    if (cleanup_error || !probe_removed) {
        throw std::runtime_error(
            "cannot remove filesystem capability probe '" + probe.string() +
            "': " +
            (cleanup_error ? cleanup_error.message() : "file was not present")
        );
    }
    if (read_error != 0) {
        if (xattr_error_is_unavailable(read_error)) {
            spdlog::debug(
                "User xattr probe could not read metadata in '{}': {}",
                root.string(),
                std::strerror(read_error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot read filesystem user extended attribute probe in '" +
            root.string() + "': " + std::strerror(read_error)
        );
    }
    if (remove_error != 0) {
        if (xattr_error_is_unavailable(remove_error)) {
            spdlog::debug(
                "User xattr probe could not remove metadata in '{}': {}",
                root.string(),
                std::strerror(remove_error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot remove filesystem user extended attribute probe in '" +
            root.string() + "': " + std::strerror(remove_error)
        );
    }
    if (!valid) {
        spdlog::debug(
            "User xattr probe did not preserve metadata in '{}'",
            root.string()
        );
        return false;
    }
    spdlog::debug("User xattr probe succeeded for '{}'", root.string());
    return true;
}

}  // namespace

bool xattr_error_is_unavailable(int error) noexcept {
    return error == ENOTSUP || error == EOPNOTSUPP || error == EPERM ||
           error == EACCES || error == ENODATA;
}

FilesystemMetadata::FilesystemMetadata(MetadataStorage storage)
    : storage_{storage} {}

FilesystemMetadata FilesystemMetadata::detect(
    config::FilesystemMetadataMode mode,
    const std::filesystem::path& root
) {
    const bool xattrs_supported =
        mode != config::FilesystemMetadataMode::database &&
        probe_xattr_support(root);
    auto metadata = from_detected_support(mode, xattrs_supported);
    spdlog::info(
        "Filesystem metadata strategy for '{}': {}",
        root.string(),
        metadata.uses_xattrs() ?
            "database journal with xattr hints" :
            "database journal"
    );
    return metadata;
}

FilesystemMetadata FilesystemMetadata::from_detected_support(
    config::FilesystemMetadataMode mode,
    bool xattrs_supported
) {
    if (mode == config::FilesystemMetadataMode::xattr &&
        !xattrs_supported) {
        throw std::runtime_error(
            "filesystem.metadata = \"xattr\" requires user extended attribute "
            "support"
        );
    }
    const auto storage =
        mode != config::FilesystemMetadataMode::database &&
                xattrs_supported ?
            MetadataStorage::database_with_xattrs :
            MetadataStorage::database;
    return FilesystemMetadata{storage};
}

bool FilesystemMetadata::uses_xattrs() const noexcept {
    return storage_ == MetadataStorage::database_with_xattrs;
}

void FilesystemMetadata::write_remote_identity(
    const graph::RemoteItem& item,
    const std::filesystem::path& path
) const {
    if (!uses_xattrs()) {
        return;
    }
    onedrive::util::UniqueFD descriptor{
        onedrive::util::open_path_no_symlinks(path, O_RDONLY)
    };
    if (::fsetxattr(
            descriptor.get(),
            "user.onedrive.remote_id",
            item.id.data(),
            item.id.size(),
            0
        ) == -1 ||
        ::fsetxattr(
            descriptor.get(),
            "user.onedrive.etag",
            item.etag.data(),
            item.etag.size(),
            0
        ) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot write synchronization metadata to '" + path.string() +
            "': " + message
        );
    }
    const std::string ticks =
        std::to_string(modified_ticks(descriptor.get()));
    if (::fsetxattr(
            descriptor.get(),
            "user.onedrive.local_modified_ticks",
            ticks.data(),
            ticks.size(),
            0
        ) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot write synchronization metadata to '" + path.string() +
            "': " + message
        );
    }
    if (const auto error = descriptor.close(); error) {
        throw std::runtime_error(
            "cannot close synchronization metadata file '" + path.string() +
            "': " + error.message()
        );
    }
}

}  // namespace onedrive::sync::detail
