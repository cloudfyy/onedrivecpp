#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_store.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <functional>

namespace onedrive::sync::detail {

void install_pending_download(
    storage::ItemStore& items,
    const SafeSyncRoot& sync_root,
    storage::PendingDownload& download,
    config::LocalConflictPolicy local_conflict,
    std::move_only_function<void(const std::filesystem::path&)> backup_created =
        {}
);

} // namespace onedrive::sync::detail
