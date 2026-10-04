#include "onedrive/path_security.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace onedrive::detail {

std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
) {
    return std::filesystem::absolute(path).lexically_normal();
}

void reject_symlink_components(
    const std::filesystem::path& path,
    std::string_view description
) {
    std::filesystem::path current;
    for (const auto& component : normalized_absolute(path)) {
        current /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            return;
        }
        if (error) {
            throw std::runtime_error(
                "cannot inspect " + std::string{description} + " path '" +
                current.string() + "': " + error.message()
            );
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                std::string{description} +
                " path contains a symbolic link: " + current.string()
            );
        }
    }
}

int open_path_no_symlinks(
    const std::filesystem::path& path,
    int flags,
    mode_t mode
) {
    open_how how{
        .flags = static_cast<__u64>(flags | O_CLOEXEC),
        .mode = static_cast<__u64>(mode),
        .resolve = RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,
    };
    const int descriptor = static_cast<int>(
        ::syscall(
            SYS_openat2,
            AT_FDCWD,
            path.c_str(),
            &how,
            sizeof(how)
        )
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot safely open path '" + path.string() + "': " +
            std::strerror(errno)
        );
    }
    return descriptor;
}

}  // namespace onedrive::detail
