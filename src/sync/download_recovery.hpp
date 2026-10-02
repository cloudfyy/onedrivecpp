#pragma once

#include "filesystem_metadata.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <string>

namespace onedrive::sync::detail {

void recover_pending_downloads(
    storage::ItemStore& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata
);

}  // namespace onedrive::sync::detail
