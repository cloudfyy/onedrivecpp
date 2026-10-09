#include "onedrive/app/queries.hpp"

#include "onedrive/app/factory.hpp"
#include "onedrive/config/config.hpp"

namespace onedrive::app {

QuotaSnapshot load_quota_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    return {
        .drive = runtime_factory.create_graph_info_client(config)->drive_info(),
    };
}

SyncStatusSnapshot load_sync_status_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    auto graph = runtime_factory.create_graph_info_client(config);
    auto identity = graph->drive_identity();
    const auto paths =
        account::AccountState::locate(config.state_directory, identity);
    const auto drive_id = identity.drive_id;
    return {
        .identity = std::move(identity),
        .sync_mode = config.sync_mode,
        .delete_policy = config.delete_policy,
        .last_run = metrics::load_sync_run_status(paths.drive_directory),
        .state = storage::read_state_summary(paths.drive_directory, drive_id),
        .websocket_enabled = config.monitor_websocket_enabled,
    };
}

} // namespace onedrive::app
