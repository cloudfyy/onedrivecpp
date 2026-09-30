#include "onedrive/sync/sync_engine.hpp"

#include <iostream>

namespace onedrive::sync {

SyncEngine::SyncEngine(
    const config::Config& config,
    graph::GraphClient& graph,
    storage::ItemDatabase& database
)
    : config_{config}, graph_{graph}, database_{database} {}

int SyncEngine::synchronize() const {
    if (config_.dry_run) {
        std::cout << "Dry run configuration:\n"
                  << "  sync directory:  " << config_.sync_directory << '\n'
                  << "  state directory: " << config_.state_directory << '\n'
                  << "  drive id:        " << config_.drive_id << '\n'
                  << "  tracked items:   " << database_.size() << '\n';
        return 0;
    }

    const auto items = graph_.list_root();
    std::cout << "Remote root contains " << items.size() << " items.\n";
    return 0;
}

}  // namespace onedrive::sync
