#include "onedrive/app/synchronization.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <utility>

namespace onedrive::app {

int synchronize_account(
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
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
    return sync::SyncEngine{
        std::move(config), *graph, *items, *metrics, &observer
    }
        .synchronize(stop_token);
}

}  // namespace onedrive::app
