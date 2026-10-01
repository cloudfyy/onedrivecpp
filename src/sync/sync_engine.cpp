#include "onedrive/sync/sync_engine.hpp"

#include <spdlog/spdlog.h>

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
                      << "  throttle retries: "
                      << config_.graph_maximum_throttle_retries << '\n'
                      << "  throttle delay:   "
                      << config_.graph_initial_throttle_delay.count() << '-'
                      << config_.graph_maximum_throttle_delay.count()
                      << " seconds\n"
                      << "  tracked items:   " << items_.size() << '\n';
            record_result(true);
            spdlog::info("Synchronization dry run completed");
            return 0;
        }

        const auto remote_items = graph_.list_root();
        std::cout << "Remote root contains " << remote_items.size() << " items.\n";
        record_result(true);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::info(
            "Synchronization inspection completed in {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        return 0;
    } catch (...) {
        record_result(false);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        throw;
    }
}

}  // namespace onedrive::sync
