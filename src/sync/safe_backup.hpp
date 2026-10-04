#pragma once

#include "local_filesystem.hpp"
#include "safe_sync_root.hpp"

#include <filesystem>
#include <string>

namespace onedrive::sync::detail {

struct SafeBackup {
    std::filesystem::path path;
    std::string fingerprint;
};

[[nodiscard]] SafeBackup preserve_safe_backup(
    const SafeSyncRoot& sync_root,
    const std::filesystem::path& source,
    const LocalFileBaseline& baseline
);

}  // namespace onedrive::sync::detail
