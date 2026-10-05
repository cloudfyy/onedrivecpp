#pragma once

#include "util/unique_file_descriptor.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>

namespace onedrive::sync::detail {

class SafePathConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CrossDeviceMoveError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FilesystemIdentity {
    std::uint64_t device{0};
    std::uint64_t inode{0};
};

class SafeSyncRoot {
public:
    explicit SafeSyncRoot(const std::filesystem::path& root);
    ~SafeSyncRoot();

    SafeSyncRoot(const SafeSyncRoot&) = delete;
    SafeSyncRoot& operator=(const SafeSyncRoot&) = delete;
    SafeSyncRoot(SafeSyncRoot&& other) noexcept;
    SafeSyncRoot& operator=(SafeSyncRoot&& other) noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::filesystem::path relative_path(
        const std::filesystem::path& path
    ) const;
    [[nodiscard]] int open(
        const std::filesystem::path& path,
        int flags,
        mode_t mode = 0
    ) const;
    [[nodiscard]] int open_directory(
        const std::filesystem::path& path
    ) const;
    void ensure_directory_tree(
        const std::filesystem::path& directory,
        bool private_permissions
    ) const;
    void rename(
        const std::filesystem::path& source,
        const std::filesystem::path& destination
    ) const;
    [[nodiscard]] bool rename_no_replace(
        const std::filesystem::path& source,
        const std::filesystem::path& destination
    ) const;
    [[nodiscard]] FilesystemIdentity identity(
        const std::filesystem::path& path,
        bool directory
    ) const;
    [[nodiscard]] bool remove(
        const std::filesystem::path& path,
        bool directory
    ) const;
    void fsync_directory(const std::filesystem::path& directory) const;

private:
    std::filesystem::path root_;
    onedrive::util::UniqueFileDescriptor descriptor_;
};

}  // namespace onedrive::sync::detail
