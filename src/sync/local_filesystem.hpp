#pragma once

#include "onedrive/storage/item_store.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace onedrive::sync::detail {

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
[[nodiscard]] std::string content_fingerprint(
    const std::filesystem::path& path
);
[[nodiscard]] std::filesystem::path temporary_path_for(
    const std::filesystem::path& destination
);
void fsync_directory(const std::filesystem::path& directory);
void ensure_directory_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& directory
);

}  // namespace onedrive::sync::detail
