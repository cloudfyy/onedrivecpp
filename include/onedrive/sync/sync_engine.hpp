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
    SyncEngine(
        const config::Config& config,
        graph::GraphClient& graph,
        storage::ItemStore& items,
        metrics::Metrics& metrics,
        const cli::Console* console = nullptr
    );

    [[nodiscard]] int synchronize() const;

private:
    gsl::not_null<const config::Config*> config_;
    gsl::not_null<graph::GraphClient*> graph_;
    gsl::not_null<storage::ItemStore*> items_;
    gsl::not_null<metrics::Metrics*> metrics_;
    const cli::Console* console_;
};

}  // namespace onedrive::sync
