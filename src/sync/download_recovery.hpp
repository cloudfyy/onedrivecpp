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

template <typename StoreImplementation>
void recover_pending_downloads(
    StoreImplementation& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata
) {
    storage::ItemStore store_proxy{items};
    recover_pending_downloads(store_proxy, sync_root, drive_id, metadata);
}

}  // namespace onedrive::sync::detail
