#pragma once

#include "onedrive/storage/item_store.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

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

[[nodiscard]] std::filesystem::path local_path_for(
    const std::filesystem::path& sync_directory,
    const std::string& remote_path
);
[[nodiscard]] std::int64_t modified_ticks(
    const std::filesystem::path& path
);
[[nodiscard]] bool local_snapshot_matches(
    const storage::ItemState& state,
    const std::filesystem::path& path
);
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
[[nodiscard]] bool paths_share_parent(
    const std::filesystem::path& left,
    const std::filesystem::path& right
);
[[nodiscard]] bool is_temporary_path_for(
    const std::filesystem::path& destination,
    const std::filesystem::path& candidate
);
void fsync_directory(const std::filesystem::path& directory);
void ensure_directory_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& directory
);

}  // namespace onedrive::sync::detail
