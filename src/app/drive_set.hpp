#pragma once

#include "onedrive/app/factory.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "operation.hpp"

#include <optional>

namespace onedrive::app::detail {

class DriveSet {
public:
    DriveSet(
        const config::Config& config,
        const RuntimeFactory& factory,
        const events::Observer& observer,
        Operation operation = Operation::synchronize
    );
    [[nodiscard]] int synchronize(
        std::stop_token stop_token, sync::PreviewCache* cache = nullptr
    );
    [[nodiscard]] std::vector<graph::GraphClient*> graph_clients() const;

private:
    struct Target {
        config::Config config;
        std::string reference;
        std::unique_ptr<graph::GraphClient> graph;
        std::optional<account::DriveIdentity> identity;
        util::UniqueFD lock;
        std::unique_ptr<storage::ItemStore> items;
        std::unique_ptr<metrics::Metrics> metrics;
    };
    void prepare(
        Target& target, std::stop_token token, const events::Observer& observer
    );

    const RuntimeFactory& factory_;
    const events::Observer& observer_;
    std::vector<Target> targets_;
    std::optional<std::string> user_id_;
    Operation operation_;
};

} // namespace onedrive::app::detail
