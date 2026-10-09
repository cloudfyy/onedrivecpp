#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/status.hpp"
#include "onedrive/sync/capabilities.hpp"

namespace onedrive::config {
struct Config;
}

namespace onedrive::app {

class RuntimeFactory;

struct QuotaSnapshot {
    graph::DriveInfo drive;
};

struct SyncStatusSnapshot {
    account::DriveIdentity identity;
    sync::SyncMode sync_mode{sync::SyncMode::bidirectional};
    sync::DeletePolicy delete_policy{sync::DeletePolicy::propagate};
    std::optional<metrics::SyncRunStatus> last_run;
    storage::StateSummary state;
    bool websocket_enabled{false};
};

[[nodiscard]] QuotaSnapshot load_quota_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
);

[[nodiscard]] SyncStatusSnapshot load_sync_status_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
);

} // namespace onedrive::app
