#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "sync/core/plan.hpp"

#include <cstddef>
#include <stop_token>
#include <string>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {
class FilesystemMetadata;
class SafeSyncRoot;
}

namespace onedrive::sync::engine_detail {

struct ExecutionSummary {
    std::size_t downloaded{0};
    std::size_t reused{0};
    std::size_t directories{0};
    std::size_t removed{0};
    std::size_t moved{0};
};

[[nodiscard]] ExecutionSummary execute_plan(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const detail::FilesystemMetadata& metadata,
    const events::Observer& observer,
    SyncCapabilities capabilities,
    std::size_t download_concurrency,
    config::TransferOrder transfer_order,
    config::LocalConflictPolicy local_conflict,
    config::SyncPermissionsMode permissions,
    const std::stop_token& stop_token
);

}  // namespace onedrive::sync::engine_detail
