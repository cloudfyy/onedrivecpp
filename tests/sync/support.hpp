#pragma once

#include "fake_graph.hpp"
#include "fake_items.hpp"

#include "onedrive/ui/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "sync/filter/selective.hpp"
#include "sync/filesystem/operations.hpp"
#include "support/sync.hpp"
#include "support/common.hpp"
#include "sync/core/delta_plan.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::test::sync {

using onedrive::test::TemporaryDirectory;

class FakeMetrics final {
public:
    void record_sync_run(
        onedrive::metrics::SyncRunOutcome outcome, std::chrono::duration<double>
    ) noexcept {
        last_outcome = outcome;
        last_success = outcome == onedrive::metrics::SyncRunOutcome::succeeded;
    }

    bool last_success{false};
    std::optional<onedrive::metrics::SyncRunOutcome> last_outcome;
};

using onedrive::test::fail;

inline onedrive::config::Config
config_for(const std::filesystem::path& root, bool dry_run) {
    auto config = onedrive::config::Config::defaults();
    config.sync_data_directory = root;
    config.state_directory = root.parent_path() / "state";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
    config.sync_mode = onedrive::sync::SyncMode::download_only;
    config.dry_run = dry_run;
    return config;
}

inline onedrive::graph::RemoteItem
file(std::string id, std::string path, std::int64_t size) {
    return {
        .id = std::move(id),
        .name = std::filesystem::path{path}.filename().string(),
        .etag = "etag",
        .parent_id = "parent",
        .remote_path = std::move(path),
        .last_modified = "2026-10-02T00:00:00Z",
        .size = size,
        .directory = false,
        .deleted = false,
    };
}

inline onedrive::storage::ItemState tracked_item(
    const std::filesystem::path& root,
    std::string id,
    std::string path,
    bool directory = false
) {
    const auto local_path = root / path;
    return {
        .drive_id = "me",
        .remote_id = std::move(id),
        .name = local_path.filename().string(),
        .etag = "etag",
        .remote_path = std::move(path),
        .local_path = local_path,
        .last_modified = "2026-10-02T00:00:00Z",
        .size = directory ? 0 : 4,
        .local_size = directory ? 0 : 4,
        .local_modified_ticks =
            directory ? 0
                      : std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::filesystem::last_write_time(local_path)
                                .time_since_epoch()
                        )
                            .count(),
        .content_hash =
            (directory || !std::filesystem::is_regular_file(
                              std::filesystem::symlink_status(local_path)
                          ))
                ? std::nullopt
                : std::optional{onedrive::util::FileHash{
                      onedrive::util::FileHashAlgorithm::sha256,
                      onedrive::sync::detail::content_fingerprint(local_path)
                  }},
        .directory = directory,
    };
}

inline onedrive::graph::RemoteItem deleted_item(std::string id) {
    return {
        .id = std::move(id),
        .deleted = true,
    };
}

} // namespace onedrive::test::sync
