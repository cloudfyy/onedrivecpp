#include "onedrive/sync/sync_engine.hpp"

#include <chrono>
#include <iostream>

namespace onedrive::sync {

SyncEngine::SyncEngine(
    const config::Config& config,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    metrics::Metrics& metrics
)
    : config_{config}, graph_{graph}, items_{items}, metrics_{metrics} {}

int SyncEngine::synchronize() const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result = [this, started_at](bool success) {
        metrics_.record_sync_run(success, std::chrono::steady_clock::now() - started_at);
    };

    try {
        if (config_.dry_run) {
            std::cout << "Dry run configuration:\n"
                      << "  sync directory:  " << config_.sync_directory << '\n'
                      << "  state directory: " << config_.state_directory << '\n'
                      << "  drive id:        " << config_.drive_id << '\n'
                      << "  tracked items:   " << items_.size() << '\n';
            record_result(true);
            return 0;
        }

        const auto remote_items = graph_.list_root();
        std::cout << "Remote root contains " << remote_items.size() << " items.\n";
        record_result(true);
        return 0;
    } catch (...) {
        record_result(false);
        throw;
    }
}

}  // namespace onedrive::sync
