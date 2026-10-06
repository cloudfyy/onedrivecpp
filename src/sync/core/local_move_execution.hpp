#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_store.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync::detail {
class ItemOperationCoordinator;
class SafeSyncRoot;
class SyncPlan;
}

namespace onedrive::sync::engine_detail {

struct MoveSummary {
    std::size_t moved{0};
    std::unordered_set<std::string> reusable_files;
    std::unordered_set<std::string> blocked;
};

MoveSummary execute_moves(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const cli::Console& console,
    config::SyncPermissionsMode permissions
);

}  // namespace onedrive::sync::engine_detail
