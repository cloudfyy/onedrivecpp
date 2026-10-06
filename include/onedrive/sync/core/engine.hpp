#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"

#include <utility>

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync {

class SyncEngine {
public:
    template <
        typename GraphImplementation,
        typename StoreImplementation,
        typename MetricsImplementation
    >
    SyncEngine(
        config::Config config,
        GraphImplementation& graph,
        StoreImplementation& items,
        MetricsImplementation& metrics,
        const cli::Console* console = nullptr
    )
        : config_{std::move(config)},
          graph_{onedrive::util::borrowed_proxy, graph},
          items_{onedrive::util::borrowed_proxy, items},
          metrics_{onedrive::util::borrowed_proxy, metrics},
          console_{console} {}

    [[nodiscard]] int synchronize();

private:
    config::Config config_;
    graph::GraphClient graph_;
    storage::ItemStore items_;
    metrics::Metrics metrics_;
    const cli::Console* console_;
};

}  // namespace onedrive::sync
