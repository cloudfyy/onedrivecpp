#include "onedrive/sync/core/engine.hpp"
#include "graph/support.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/sha256.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <tuple>

namespace onedrive::sync {

PreviewCache::Scope PreviewCache::scope(const config::Config& config) {
    return {
        .sync_root = util::normalized_absolute(config.sync_data_directory),
        .state_root = util::normalized_absolute(config.state_directory),
        .drive_id = config.drive_id,
        .graph_endpoint = config.graph_endpoint,
        .application_id = config.application_id,
        .tenant = config.azure_tenant_id,
        .auth_endpoint = config.auth_endpoint,
        .auth_scope = config.auth_scope,
        .validation = config.download_validation,
    };
}

std::optional<PreviewCache::State> PreviewCache::capture_state(
    const storage::ItemStore& items, const std::string& drive
) {
    if (!items.pending_downloads(drive).empty() ||
        !items.partial_downloads(drive).empty() ||
        !items.pending_uploads(drive).empty() ||
        !items.pending_deletes(drive).empty() ||
        !items.pending_remote_moves(drive).empty() ||
        !items.pending_moves(drive).empty()) {
        return std::nullopt;
    }
    State state{
        .cursor = items.delta_link(drive),
        .filter = items.sync_filter_fingerprint(drive),
        .items = items.drive_items(drive),
        .blocked = items.blocked_items(drive),
        .suppressions = items.upload_suppressions(drive),
    };
    std::ranges::sort(state.items, {}, &storage::ItemState::remote_id);
    std::ranges::sort(state.blocked, {}, &storage::BlockedItem::remote_id);
    std::ranges::sort(state.suppressions, {}, [](const auto& suppression) {
        return std::tie(suppression.remote_id, suppression.local_path);
    });
    return state;
}

std::string PreviewCache::scope_fingerprint(const config::Config& config) {
    const auto value = scope(config);
    return util::sha256_hex(
        nlohmann::json::array(
            {value.sync_root.string(),
             value.state_root.string(),
             value.drive_id,
             value.graph_endpoint,
             value.application_id,
             value.tenant,
             value.auth_endpoint,
             value.auth_scope,
             static_cast<int>(value.validation),
             "graph-quickxor-sha1-v1",
             graph::client_detail::unsupported_format_policy()}
        )
            .dump()
    );
}

} // namespace onedrive::sync
