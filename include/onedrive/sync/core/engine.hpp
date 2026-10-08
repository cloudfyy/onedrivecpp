#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"

#include <gsl/pointers>

#include <utility>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync {

class SyncEngine {
public:
    template <
        typename GraphImplementation,
        typename StoreImplementation,
        typename MetricsImplementation>
    SyncEngine(
        config::Config config,
        GraphImplementation& graph,
        StoreImplementation& items,
        MetricsImplementation& metrics,
        gsl::not_null<const events::Observer*> observer
    )
        : config_{std::move(config)},
          graph_{onedrive::util::borrowed_proxy, graph},
          items_{onedrive::util::borrowed_proxy, items},
          metrics_{onedrive::util::borrowed_proxy, metrics},
          observer_{observer} {
    }

    [[nodiscard]] int synchronize();

private:
    config::Config config_;
    graph::GraphClient graph_;
    storage::ItemStore items_;
    metrics::Metrics metrics_;
    gsl::not_null<const events::Observer*> observer_;
};

} // namespace onedrive::sync
