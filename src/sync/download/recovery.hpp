#pragma once

#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <string>

namespace onedrive::sync::detail {

void recover_pending_downloads(
    storage::ItemStore& items,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    bool private_permissions = true
);
void recover_pending_downloads(
    storage::ItemStore& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    bool private_permissions = true
);

template <typename StoreImplementation>
void recover_pending_downloads(
    StoreImplementation& items,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    bool private_permissions = true
) {
    storage::ItemStore store_proxy{onedrive::util::borrowed_proxy, items};
    recover_pending_downloads(
        store_proxy,
        sync_root,
        drive_id,
        metadata,
        private_permissions
    );
}

template <typename StoreImplementation>
void recover_pending_downloads(
    StoreImplementation& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    bool private_permissions = true
) {
    storage::ItemStore store_proxy{onedrive::util::borrowed_proxy, items};
    recover_pending_downloads(
        store_proxy,
        sync_root,
        drive_id,
        metadata,
        private_permissions
    );
}

}  // namespace onedrive::sync::detail
