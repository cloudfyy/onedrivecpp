#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_store.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace onedrive::sync::detail {

class InvalidRemotePathError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LocalPathConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LocalModificationConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LocalFileBaseline {
    bool existed{false};
    std::int64_t size{0};
    std::int64_t modified_ticks{0};
    std::string fingerprint;
};

enum class MissingPathPolicy {
    ignore,
    report,
};

[[nodiscard]] std::filesystem::path prepare_sync_root(
    const std::filesystem::path& configured_root,
    config::SyncPermissionsMode permissions
);
[[nodiscard]] std::filesystem::path local_path_for(
    const std::filesystem::path& sync_data_directory,
    const std::string& remote_path
);
[[nodiscard]] std::int64_t modified_ticks(
    const std::filesystem::path& path
);
[[nodiscard]] std::int64_t modified_ticks(int descriptor);
bool remove_no_symlinks(
    const std::filesystem::path& path,
    MissingPathPolicy missing_path = MissingPathPolicy::ignore
);
void apply_remote_modified_time(
    const std::filesystem::path& path,
    std::string_view remote_modified
);
[[nodiscard]] bool local_snapshot_matches(
    const storage::ItemState& state,
    const std::filesystem::path& path
);
[[nodiscard]] bool remote_content_version_matches(
    const storage::ItemState& previous,
    std::string_view etag,
    std::string_view ctag
) noexcept;
[[nodiscard]] LocalFileBaseline capture_local_file_baseline(
    const std::filesystem::path& path
);
[[nodiscard]] bool local_file_matches_baseline(
    const std::filesystem::path& path,
    const LocalFileBaseline& baseline
);
[[nodiscard]] std::string content_fingerprint(
    const std::filesystem::path& path
);
[[nodiscard]] std::filesystem::path temporary_path_for(
    const std::filesystem::path& destination
);
[[nodiscard]] std::filesystem::path move_staging_path_for(
    const std::filesystem::path& source
);
[[nodiscard]] bool paths_share_parent(
    const std::filesystem::path& left,
    const std::filesystem::path& right
);
[[nodiscard]] bool is_temporary_path_for(
    const std::filesystem::path& destination,
    const std::filesystem::path& candidate
);
void fsync_file(const std::filesystem::path& path);
void fsync_directory(const std::filesystem::path& directory);
void ensure_directory_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    config::SyncPermissionsMode permissions =
        config::SyncPermissionsMode::private_access
);

}  // namespace onedrive::sync::detail
