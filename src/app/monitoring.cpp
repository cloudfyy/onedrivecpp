#include "onedrive/app/monitoring.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <format>
#include <utility>

namespace onedrive::app {

int monitor_account(
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    bool keyboard_exit,
    std::stop_token stop_token
) {
    auto graph = runtime_factory.create_graph_client(config);
    const auto identity = graph->drive_identity();
    config.drive_id = identity.drive_id;
    config.sync_data_directory = account::AccountState::drive_data_directory(
        config.sync_data_directory, identity
    );
    auto items = runtime_factory.create_item_store(config, identity);
    items->open();
    auto metrics = runtime_factory.create_metrics(config, identity);

    observer.message(
        events::MessageKind::success,
        "monitor_ready",
        "Monitoring local and Microsoft Graph changes for: " +
            config.sync_data_directory.string()
    );
    observer.message(
        events::MessageKind::information,
        "monitor_status",
        config.monitor_websocket_enabled ?
            std::format(
                "Local changes settle for {} milliseconds; remote "
                "WebSocket notifications trigger Delta synchronization, "
                "with Graph polling every {} seconds as fallback.",
                config.monitor_settle_delay.count(),
                config.monitor_poll_interval.count()
            ) :
            std::format(
                "Local changes settle for {} milliseconds; remote "
                "WebSocket notifications are disabled, and Graph is polled "
                "every {} seconds.",
                config.monitor_settle_delay.count(),
                config.monitor_poll_interval.count()
            )
    );

    monitor::SyncCallback synchronize{
        [&config, &graph, &items, &metrics, &observer](
            const std::stop_token& sync_stop_token
        ) {
            return sync::SyncEngine{
                config, *graph, *items, *metrics, &observer
            }
                .synchronize(sync_stop_token);
        }
    };
    auto monitor = runtime_factory.create_monitor(
        config, std::move(synchronize), *graph
    );
    return monitor->run(keyboard_exit, stop_token);
}

}  // namespace onedrive::app
