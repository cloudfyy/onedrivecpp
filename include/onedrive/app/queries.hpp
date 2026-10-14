#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/status.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>

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

// Local diagnostics only: does not contact Graph or create/migrate state.
[[nodiscard]] std::vector<storage::BlockedItem>
load_blocked_items(const config::Config& config);

// Frontends retain their own text case/Unicode rules, but share field
// selection.
template <std::predicate<const std::string&> TextMatches>
[[nodiscard]] bool matches_blocked_item(
    const storage::BlockedItem& item,
    std::optional<std::string_view> reason,
    TextMatches text_matches
) {
    return (!reason || *reason == item.reason_code) &&
           (text_matches(item.remote_path) || text_matches(item.reason_code) ||
            text_matches(item.reason_message));
}

} // namespace onedrive::app
