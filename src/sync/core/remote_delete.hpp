#pragma once

#include "onedrive/storage/item_store.hpp"

#include <cstddef>
#include <string>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {
class ItemOperationCoordinator;
class SafeSyncRoot;
class SyncPlan;
}

namespace onedrive::sync::engine_detail {

std::size_t execute_removals(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const events::Observer& observer
);

}  // namespace onedrive::sync::engine_detail
