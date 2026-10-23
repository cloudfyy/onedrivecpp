#include "onedrive/app/queries.hpp"

#include "onedrive/app/factory.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/util/path_security.hpp"
#include <iterator>

namespace onedrive::app {

std::vector<storage::BlockedItem>
load_blocked_items(const config::Config& config) {
    if (config.drive_ids.size() > 1) {
        std::vector<storage::BlockedItem> result;
        for (const auto& id : config.drive_ids) {
            auto items = load_blocked_items(config.for_drive(id));
            result.insert(
                result.end(),
                std::make_move_iterator(items.begin()),
                std::make_move_iterator(items.end())
            );
        }
        return result;
    }
    const auto saved = account::AccountState::saved_drive(
        config.state_directory, config.drive_id
    );
    if (!saved) {
        return {};
    }
    const auto database_path = saved->directory / "items.sqlite3";
    util::reject_symlink_components(database_path, "state database");
    if (!std::filesystem::exists(database_path)) {
        return {};
    }
    storage::ItemDatabase database{saved->directory, saved->identity};
    database.open_read_only();
    return database.blocked_items(saved->identity.drive_id);
}

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
