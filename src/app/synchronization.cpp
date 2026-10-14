#include "onedrive/app/synchronization.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "operation_state.hpp"
#include "lock.hpp"
#include "checks.hpp"
#include "onedrive/graph/graph_client.hpp"

#include <utility>

namespace onedrive::app {

int synchronize_account(
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    std::stop_token stop_token,
    sync::PreviewCache* preview_cache
) {
    detail::OperationStateMachine state{
        observer, events::OperationKind::synchronization
    };
    const auto clear_preview = [preview_cache] {
        if (preview_cache) {
            preview_cache->clear();
        }
    };
    const auto finish_cancelled = [&] {
        clear_preview();
        state.transition(events::OperationState::stopping);
        state.transition(events::OperationState::ready);
        return exit_code::cancelled;
    };
    try {
        state.transition(events::OperationState::syncing);
    } catch (...) {
        // A failed observer leaves the initial transition uncommitted.
        clear_preview();
        throw;
    }
    try {
        if (stop_token.stop_requested()) {
            return finish_cancelled();
        }
        auto graph = runtime_factory.create_graph_client(config);
        const auto identity = graph->drive_identity(stop_token);
        if (stop_token.stop_requested()) {
            return finish_cancelled();
        }
        config.drive_id = identity.drive_id;
        config.sync_data_directory =
            account::AccountState::drive_data_directory(
                config.sync_data_directory, identity
            );
        const auto sync_lock = detail::acquire_sync_directory_lock(
            config.sync_data_directory,
            config.state_directory,
            "synchronize",
            observer
        );
        detail::prepare_sync_directory(config, detail::Operation::synchronize);
        auto items = runtime_factory.create_item_store(config, identity);
        items->open();
        auto metrics = runtime_factory.create_metrics(config, identity);
        const int result =
            sync::SyncEngine{
                std::move(config),
                *graph,
                *items,
                *metrics,
                &observer,
                preview_cache
            }
                .synchronize(stop_token);
        if (result == exit_code::cancelled) {
            return finish_cancelled();
        }
        state.transition(
            result == exit_code::success ? events::OperationState::ready
                                         : events::OperationState::failed
        );
        return result;
    } catch (const graph::RequestCancelledError&) {
        return finish_cancelled();
    } catch (...) {
        clear_preview();
        state.transition(events::OperationState::failed);
        throw;
    }
}

}  // namespace onedrive::app
