#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"

#include <gsl/pointers>

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
        const config::Config& config,
        GraphImplementation& graph,
        StoreImplementation& items,
        MetricsImplementation& metrics,
        const cli::Console* console = nullptr
    )
        : config_{&config},
          graph_{onedrive::detail::borrowed_proxy, graph},
          items_{onedrive::detail::borrowed_proxy, items},
          metrics_{onedrive::detail::borrowed_proxy, metrics},
          console_{console} {}

    [[nodiscard]] int synchronize() const;

private:
    gsl::not_null<const config::Config*> config_;
    mutable graph::GraphClient graph_;
    mutable storage::ItemStore items_;
    mutable metrics::Metrics metrics_;
    const cli::Console* console_;
};

}  // namespace onedrive::sync
