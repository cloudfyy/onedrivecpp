#pragma once

#include "onedrive/config/config.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <string>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {

void recover_pending_downloads(
    storage::ItemStore& items,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    config::SyncPermissionsMode permissions =
        config::SyncPermissionsMode::private_access,
    const events::Observer* observer = nullptr,
    config::LocalConflictPolicy local_conflict =
        config::LocalConflictPolicy::block
);
void recover_pending_downloads(
    storage::ItemStore& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    config::SyncPermissionsMode permissions =
        config::SyncPermissionsMode::private_access,
    const events::Observer* observer = nullptr,
    config::LocalConflictPolicy local_conflict =
        config::LocalConflictPolicy::block
);

template <typename StoreImplementation>
void recover_pending_downloads(
    StoreImplementation& items,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    config::SyncPermissionsMode permissions =
        config::SyncPermissionsMode::private_access,
    const events::Observer* observer = nullptr,
    config::LocalConflictPolicy local_conflict =
        config::LocalConflictPolicy::block
) {
    storage::ItemStore store_proxy{onedrive::util::borrowed_proxy, items};
    recover_pending_downloads(
        store_proxy,
        sync_root,
        drive_id,
        metadata,
        permissions,
        observer,
        local_conflict
    );
}

template <typename StoreImplementation>
void recover_pending_downloads(
    StoreImplementation& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata,
    config::SyncPermissionsMode permissions =
        config::SyncPermissionsMode::private_access,
    const events::Observer* observer = nullptr,
    config::LocalConflictPolicy local_conflict =
        config::LocalConflictPolicy::block
) {
    storage::ItemStore store_proxy{onedrive::util::borrowed_proxy, items};
    recover_pending_downloads(
        store_proxy,
        sync_root,
        drive_id,
        metadata,
        permissions,
        observer,
        local_conflict
    );
}

}  // namespace onedrive::sync::detail
