#include "util/private_file.hpp"

#include "onedrive/util/path_security.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/system_error.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <unistd.h>

namespace onedrive::util {
namespace {

[[noreturn]] void throw_invalid_permissions(
    const std::filesystem::path& path,
    std::string_view description,
    std::string_view requirement
) {
    throw std::runtime_error(
        std::string{description} + " file " + std::string{requirement} + ": " +
        path.string()
    );
}

} // namespace

std::string read_private_file(
    const std::filesystem::path& path,
    std::string_view description,
    PrivateFileRequirements requirements
) {
    const UniqueFD descriptor{
        open_path_no_symlinks(path, O_RDONLY | O_NONBLOCK)
    };
    return read_private_file(descriptor.get(), path, description, requirements);
}

std::string read_private_file(
    int descriptor,
    const std::filesystem::path& path,
    std::string_view description,
    PrivateFileRequirements requirements
) {
    struct stat status{};
    if (::fstat(descriptor, &status) == -1) {
        throw_errno_error(
            "cannot inspect " + std::string{description} + " file '" +
            path.string() + "'"
        );
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error(
            std::string{description} +
            " path is not a regular file: " + path.string()
        );
    }
    if (requirements.require_single_link && status.st_nlink != 1) {
        throw std::runtime_error(
            std::string{description} +
            " file must have exactly one hard link: " + path.string()
        );
    }
    if (requirements.required_owner &&
        status.st_uid != *requirements.required_owner) {
        throw_invalid_permissions(
            path, description, requirements.permission_requirement
        );
    }
    const mode_t permissions = status.st_mode & 07777;
    if ((requirements.exact_permissions &&
         permissions != *requirements.exact_permissions) ||
        (permissions & requirements.forbidden_permissions) != 0) {
        throw_invalid_permissions(
            path, description, requirements.permission_requirement
        );
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) >
                                  requirements.maximum_size) {
        throw std::runtime_error(
            std::format(
                "{} file exceeds the {} byte size limit: {}",
                description,
                requirements.maximum_size,
                path.string()
            )
        );
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(status.st_size));
    constexpr std::size_t read_buffer_bytes = 4096;
    std::array<char, read_buffer_bytes> buffer{};
    while (true) {
        const auto count = ::pread(
            descriptor,
            buffer.data(),
            buffer.size(),
            static_cast<off_t>(contents.size())
        );
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1) {
            throw_errno_error(
                "cannot read " + std::string{description} + " file '" +
                path.string() + "'"
            );
        }
        if (count == 0) {
            break;
        }
        if (contents.size() + static_cast<std::size_t>(count) >
            requirements.maximum_size) {
            throw std::runtime_error(
                std::format(
                    "{} file exceeds the {} byte size limit: {}",
                    description,
                    requirements.maximum_size,
                    path.string()
                )
            );
        }
        contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return contents;
}

} // namespace onedrive::util
