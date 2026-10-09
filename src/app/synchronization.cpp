#include "onedrive/app/synchronization.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "operation_state.hpp"

#include <utility>

namespace onedrive::app {

int synchronize_account(
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    std::stop_token stop_token
) {
    detail::OperationStateMachine state{
        observer, events::OperationKind::synchronization
    };
    state.transition(events::OperationState::syncing);
    try {
        auto graph = runtime_factory.create_graph_client(config);
        const auto identity = graph->drive_identity();
        config.drive_id = identity.drive_id;
        config.sync_data_directory =
            account::AccountState::drive_data_directory(
                config.sync_data_directory, identity
            );
        auto items = runtime_factory.create_item_store(config, identity);
        items->open();
        auto metrics = runtime_factory.create_metrics(config, identity);
        const int result = sync::SyncEngine{
            std::move(config), *graph, *items, *metrics, &observer
        }
                                 .synchronize(stop_token);
        if (result == 130) {
            state.transition(events::OperationState::stopping);
        }
        state.transition(
            result == 0 || result == 130 ?
                events::OperationState::ready :
                events::OperationState::failed
        );
        return result;
    } catch (...) {
        state.transition(events::OperationState::failed);
        throw;
    }
}

}  // namespace onedrive::app
