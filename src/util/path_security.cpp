#include "onedrive/util/path_security.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/syscall.h>
#include <unistd.h>

namespace onedrive::util {

std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
) {
    // Normalize only the path syntax; canonicalization would resolve symlinks
    // before callers have a chance to reject them.
    return std::filesystem::absolute(path).lexically_normal();
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
            // Once a component is missing, none of its descendants can exist
            // yet, so there are no remaining symlinks to inspect.
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

UniqueFD open_path_no_symlinks(
    const std::filesystem::path& path,
    int flags,
    mode_t mode
) {
    open_how how{
        .flags = static_cast<__u64>(flags | O_CLOEXEC),
        .mode = static_cast<__u64>(mode),
        // Enforce the check during path resolution to avoid a race between a
        // separate symlink check and opening the file.
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
    if (descriptor == -1 && errno == ENOSYS) {
        throw std::runtime_error(
            "cannot safely open path '" + path.string() +
            "': openat2 is unavailable; Linux 5.6 or newer "
            "(or a kernel with openat2 backported) is required"
        );
    }
    if (descriptor == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "cannot safely open path '" + path.string() + "'"
        );
    }
    return UniqueFD{descriptor};
}

}  // namespace onedrive::util
