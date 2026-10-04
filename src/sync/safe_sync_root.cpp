#include "safe_sync_root.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace onedrive::sync::detail {
namespace {

class Descriptor {
public:
    explicit Descriptor(int value = -1) noexcept : value_{value} {}
    ~Descriptor() {
        if (value_ != -1) {
            ::close(value_);
        }
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept
        : value_{std::exchange(other.value_, -1)} {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ != -1) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return value_;
    }

private:
    int value_;
};

int open_beneath(
    int directory,
    const std::filesystem::path& relative,
    int flags,
    mode_t mode
) {
    const std::string value = relative.empty() ? "." : relative.string();
    open_how how{
        .flags = static_cast<__u64>(flags),
        .mode = static_cast<__u64>(mode),
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
                   RESOLVE_NO_SYMLINKS,
    };
    const int descriptor = static_cast<int>(
        ::syscall(SYS_openat2, directory, value.c_str(), &how, sizeof(how))
    );
    if (descriptor == -1 && errno == ENOSYS) {
        throw std::runtime_error(
            "cannot safely resolve synchronization path: openat2 is "
            "unavailable; Linux 5.6 or newer (or a kernel with openat2 "
            "backported) is required"
        );
    }
    return descriptor;
}

}  // namespace

SafeSyncRoot::SafeSyncRoot(const std::filesystem::path& root)
    : root_{std::filesystem::absolute(root).lexically_normal()},
      descriptor_{::open(
          root_.c_str(),
          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      )} {
    if (descriptor_ == -1) {
        throw std::runtime_error(
            "cannot open synchronization root '" + root_.string() + "': " +
            std::strerror(errno)
        );
    }
}

SafeSyncRoot::~SafeSyncRoot() {
    if (descriptor_ != -1) {
        ::close(descriptor_);
    }
}

SafeSyncRoot::SafeSyncRoot(SafeSyncRoot&& other) noexcept
    : root_{std::move(other.root_)},
      descriptor_{std::exchange(other.descriptor_, -1)} {}

SafeSyncRoot& SafeSyncRoot::operator=(SafeSyncRoot&& other) noexcept {
    if (this != &other) {
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
        root_ = std::move(other.root_);
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

const std::filesystem::path& SafeSyncRoot::path() const noexcept {
    return root_;
}

std::filesystem::path SafeSyncRoot::relative_path(
    const std::filesystem::path& path
) const {
    const auto normalized =
        std::filesystem::absolute(path).lexically_normal();
    const auto relative = normalized.lexically_relative(root_);
    if (relative.empty() && normalized != root_) {
        throw std::runtime_error(
            "local synchronization path escapes the configured directory"
        );
    }
    if (relative.is_absolute()) {
        throw std::runtime_error(
            "local synchronization path escapes the configured directory"
        );
    }
    for (const auto& component : relative) {
        if (component == "..") {
            throw std::runtime_error(
                "local synchronization path escapes the configured directory"
            );
        }
    }
    return relative;
}

int SafeSyncRoot::open(
    const std::filesystem::path& path,
    int flags,
    mode_t mode
) const {
    const auto relative = relative_path(path);
    const int descriptor = open_beneath(
        descriptor_,
        relative,
        flags | O_CLOEXEC,
        mode
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot safely open synchronization path '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    return descriptor;
}

int SafeSyncRoot::open_directory(
    const std::filesystem::path& path
) const {
    return open(path, O_RDONLY | O_DIRECTORY);
}

void SafeSyncRoot::ensure_directory_tree(
    const std::filesystem::path& directory,
    bool private_permissions
) const {
    const auto relative = relative_path(directory);
    Descriptor current{::fcntl(descriptor_, F_DUPFD_CLOEXEC, 0)};
    if (current.get() == -1) {
        throw std::runtime_error(
            "cannot duplicate synchronization root descriptor: " +
            std::string{std::strerror(errno)}
        );
    }
    for (const auto& component : relative) {
        if (::mkdirat(
                current.get(),
                component.c_str(),
                private_permissions ? S_IRWXU : S_IRWXU | S_IRWXG | S_IRWXO
            ) == -1 &&
            errno != EEXIST) {
            throw std::runtime_error(
                "cannot create local synchronization directory '" +
                (root_ / relative).string() + "': " + std::strerror(errno)
            );
        }
        Descriptor next{open_beneath(
            current.get(),
            component,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC,
            0
        )};
        if (next.get() == -1) {
            throw SafePathConflictError(
                "local synchronization path is not a safe directory: " +
                (root_ / relative).string() + ": " + std::strerror(errno)
            );
        }
        if (private_permissions && ::fchmod(next.get(), S_IRWXU) == -1) {
            throw std::runtime_error(
                "cannot secure local synchronization directory '" +
                (root_ / relative).string() + "': " + std::strerror(errno)
            );
        }
        current = std::move(next);
    }
}

void SafeSyncRoot::rename(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) const {
    const auto source_relative = relative_path(source);
    const auto destination_relative = relative_path(destination);
    Descriptor source_parent{open_beneath(
        descriptor_,
        source_relative.parent_path(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        0
    )};
    Descriptor destination_parent{open_beneath(
        descriptor_,
        destination_relative.parent_path(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        0
    )};
    if (source_parent.get() == -1 || destination_parent.get() == -1) {
        throw std::runtime_error(
            "cannot safely open synchronization directory for rename: " +
            std::string{std::strerror(errno)}
        );
    }

    if (::renameat(
            source_parent.get(),
            source_relative.filename().c_str(),
            destination_parent.get(),
            destination_relative.filename().c_str()
        ) == -1) {
        throw std::runtime_error(
            "cannot install synchronized file '" + destination.string() +
            "': " + std::strerror(errno)
        );
    }
}

bool SafeSyncRoot::rename_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) const {
    const auto source_relative = relative_path(source);
    const auto destination_relative = relative_path(destination);
    Descriptor source_parent{open_beneath(
        descriptor_,
        source_relative.parent_path(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        0
    )};
    Descriptor destination_parent{open_beneath(
        descriptor_,
        destination_relative.parent_path(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        0
    )};
    if (source_parent.get() == -1 || destination_parent.get() == -1) {
        throw std::runtime_error(
            "cannot safely open synchronization directory for safeBackup: " +
            std::string{std::strerror(errno)}
        );
    }
    if (::renameat2(
            source_parent.get(),
            source_relative.filename().c_str(),
            destination_parent.get(),
            destination_relative.filename().c_str(),
            RENAME_NOREPLACE
        ) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return false;
    }
    throw std::runtime_error(
        "cannot install safeBackup '" + destination.string() + "': " +
        std::strerror(errno)
    );
}

void SafeSyncRoot::fsync_directory(
    const std::filesystem::path& directory
) const {
    Descriptor descriptor{open_directory(directory)};
    if (::fsync(descriptor.get()) == -1) {
        throw std::runtime_error(
            "cannot flush synchronization directory '" + directory.string() +
            "': " + std::strerror(errno)
        );
    }
}

}  // namespace onedrive::sync::detail
