#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync {

class SyncEngine {
public:
    SyncEngine(
        const config::Config& config,
        graph::GraphClient& graph,
        storage::ItemStore& items,
        metrics::Metrics& metrics,
        const cli::Console* console = nullptr
    );

    [[nodiscard]] int synchronize() const;

private:
    const config::Config& config_;
    graph::GraphClient& graph_;
    storage::ItemStore& items_;
    metrics::Metrics& metrics_;
    const cli::Console* console_;
};

}  // namespace onedrive::sync
