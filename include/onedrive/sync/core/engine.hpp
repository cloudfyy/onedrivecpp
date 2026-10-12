#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/item_store.hpp"

#include <gsl/pointers>

#include <stop_token>
#include <utility>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync {

// Session-only remote metadata, never an executable plan or an applied cursor.
// Access requires the same exclusion as synchronization itself.
class PreviewCache {
public:
    void clear() noexcept {
        snapshot_.reset();
    }
    [[nodiscard]] bool available() const noexcept {
        return snapshot_.has_value();
    }

private:
    friend class SyncEngine;
    struct State {
        std::optional<std::string> cursor;
        std::optional<std::string> filter;
        std::vector<storage::ItemState> items;
        std::vector<storage::BlockedItem> blocked;
        std::vector<storage::UploadSuppression> suppressions;

        bool operator==(const State&) const = default;
    };
    struct Scope {
        std::filesystem::path sync_root;
        std::filesystem::path state_root;
        std::string drive_id;
        std::string graph_endpoint;
        std::string application_id;
        std::string tenant;
        std::string auth_endpoint;
        std::string auth_scope;
        config::DownloadValidationMode validation;

        bool operator==(const Scope&) const = default;
    };
    struct Snapshot {
        Scope scope;
        std::string filter;
        State state;
        graph::DeltaResult delta;
        storage::DeltaApplyMode apply_mode;
    };
    [[nodiscard]] static Scope scope(const config::Config& config);
    [[nodiscard]] static std::optional<State>
    capture_state(const storage::ItemStore& items, const std::string& drive);
    std::optional<Snapshot> snapshot_;
};

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
        gsl::not_null<const events::Observer*> observer,
        PreviewCache* preview_cache = nullptr
    )
        : config_{std::move(config)},
          graph_{onedrive::util::borrowed_proxy, graph},
          items_{onedrive::util::borrowed_proxy, items},
          metrics_{onedrive::util::borrowed_proxy, metrics},
          observer_{observer},
          preview_cache_{preview_cache} {
    }

    [[nodiscard]] int synchronize(std::stop_token stop_token = {});

private:
    config::Config config_;
    graph::GraphClient graph_;
    storage::ItemStore items_;
    metrics::Metrics metrics_;
    gsl::not_null<const events::Observer*> observer_;
    PreviewCache* preview_cache_{};
};

} // namespace onedrive::sync
