#include "onedrive/sync/sync_engine.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace onedrive::sync {
namespace {

std::filesystem::path local_path_for(
    const std::filesystem::path& sync_directory,
    const std::string& remote_path
) {
    const std::filesystem::path relative_path{remote_path};
    if (relative_path.empty() || relative_path.is_absolute()) {
        throw std::runtime_error("Microsoft Graph returned an invalid remote path");
    }
    for (const auto& component : relative_path) {
        if (component == "." || component == "..") {
            throw std::runtime_error(
                "Microsoft Graph returned an unsafe remote path"
            );
        }
    }
    return sync_directory / relative_path;
}

}  // namespace

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
        }

        const auto previous_delta_link = items_.delta_link(config_.drive_id);
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_.drive_id,
            items_.size(),
            previous_delta_link ? "present" : "absent"
        );
        const auto delta = graph_.list_delta(previous_delta_link);
        storage::ItemDelta state_delta{
            .drive_id = config_.drive_id,
            .upserts = {},
            .removals = {},
            .delta_link = delta.delta_link,
        };
        for (const auto& item : delta.changes) {
            if (item.deleted) {
                state_delta.removals.push_back(item.id);
                spdlog::trace("Remote item deleted: id='{}'", item.id);
                continue;
            }
            spdlog::trace(
                "Remote item changed: path='{}', id='{}', eTag='{}', type={}",
                item.remote_path,
                item.id,
                item.etag,
                item.directory ? "directory" : "file"
            );
            state_delta.upserts.push_back({
                .drive_id = config_.drive_id,
                .remote_id = item.id,
                .parent_id = item.parent_id,
                .name = item.name,
                .etag = item.etag,
                .remote_path = item.remote_path,
                .local_path =
                    local_path_for(config_.sync_directory, item.remote_path),
                .last_modified = item.last_modified,
                .size = item.size,
                .directory = item.directory,
            });
        }

        std::cout << "Remote delta contains " << delta.changes.size()
                  << " changes (" << state_delta.upserts.size() << " upserts, "
                  << state_delta.removals.size() << " removals).\n";
        spdlog::info(
            "Remote delta prepared for drive '{}': {} upserts, {} removals",
            config_.drive_id,
            state_delta.upserts.size(),
            state_delta.removals.size()
        );
        if (config_.dry_run) {
            spdlog::debug(
                "Dry run left synchronization state unchanged for drive '{}'",
                config_.drive_id
            );
        } else {
            spdlog::debug(
                "Persisting remote delta for drive '{}'",
                config_.drive_id
            );
            items_.apply_delta(std::move(state_delta));
        }
        record_result(true);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        if (config_.dry_run) {
            spdlog::info(
                "Synchronization dry run completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
        } else {
            spdlog::info(
                "Synchronization state update completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
        }
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
