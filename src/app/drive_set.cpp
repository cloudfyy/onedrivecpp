#include "drive_set.hpp"

#include "checks.hpp"
#include "lock.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <format>
#include <exception>
#include <unordered_set>

namespace onedrive::app::detail {
namespace {

class DriveObserver final : public events::Observer {
public:
    explicit DriveObserver(const events::Observer& target)
        : target_{target} {
    }

    void rethrow_failure() const {
        auto guard = lock();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void on_event(const events::Event& event) const override {
        try {
            target_.publish(event);
        } catch (...) {
            if (!failure_) {
                failure_ = std::current_exception();
            }
            throw;
        }
    }

    const events::Observer& target_;
    mutable std::exception_ptr failure_;
};

} // namespace

DriveSet::DriveSet(
    const config::Config& config,
    const RuntimeFactory& factory,
    const events::Observer& observer,
    Operation operation
)
    : factory_{factory},
      observer_{observer},
      operation_{operation} {
    for (const auto& id : config.configured_drive_ids()) {
        auto selected = config.for_drive(id);
        auto graph = factory_.create_graph_client(selected);
        targets_.push_back({
            .config = std::move(selected),
            .reference = id,
            .graph = std::move(graph),
            .identity = std::nullopt,
            .lock = {},
            .items = {},
            .metrics = {},
        });
    }
}

void DriveSet::prepare(
    Target& target, std::stop_token token, const events::Observer& observer
) {
    if (target.metrics) {
        return;
    }
    if (token.stop_requested()) {
        throw graph::RequestCancelledError{"Drive preparation cancelled"};
    }
    const auto& identity = *target.identity;
    auto selected = target.config;
    selected.drive_id = identity.drive_id;
    selected.sync_data_directory = account::AccountState::drive_data_directory(
        target.config.sync_data_directory, identity
    );
    if (!target.lock) {
        target.lock = acquire_sync_directory_lock(
            selected.sync_data_directory,
            selected.state_directory,
            operation_name(operation_),
            observer
        );
    }
    prepare_sync_directory(selected, Operation::synchronize);
    auto items = factory_.create_item_store(selected, identity);
    items->open();
    auto metrics = factory_.create_metrics(selected, identity);
    target.config = std::move(selected);
    target.items = std::move(items);
    target.metrics = std::move(metrics);
}

int DriveSet::synchronize(
    std::stop_token stop_token, sync::PreviewCache* cache
) {
    int aggregate = exit_code::success;
    std::size_t failed = 0;
    std::size_t blocked = 0;
    std::size_t completed = 0;
    DriveObserver observer{observer_};
    std::unordered_set<std::string> seen;
    if (cache) {
        std::vector<std::string> references;
        for (const auto& target : targets_) {
            references.push_back(target.reference);
        }
        cache->select_drives(references);
    }
    for (auto& target : targets_) {
        if (stop_token.stop_requested()) {
            if (cache) {
                cache->clear();
            }
            return exit_code::cancelled;
        }
        int result = exit_code::failure;
        std::string error;
        try {
            // Resolve aliases before acquiring a second lock or applying
            // changes.
            if (!target.identity) {
                auto identity = target.graph->drive_identity(stop_token);
                if (user_id_ && *user_id_ != identity.user_id) {
                    throw graph::AccountAuthenticationError{
                        "configured Drives must use the same signed-in "
                        "Microsoft account"
                    };
                }
                user_id_ = identity.user_id;
                target.identity = std::move(identity);
            }
            if (!seen.insert(target.identity->drive_id).second) {
                throw std::runtime_error{
                    "multiple configured references resolve to the same Drive"
                };
            }
            prepare(target, stop_token, observer);
            auto* preview =
                cache ? &cache->for_drive(target.reference) : nullptr;
            result =
                sync::SyncEngine{
                    target.config,
                    *target.graph,
                    *target.items,
                    *target.metrics,
                    &observer,
                    preview
                }
                    .synchronize(stop_token);
            observer.rethrow_failure();
        } catch (const graph::RequestCancelledError&) {
            if (cache) {
                cache->clear();
            }
            return exit_code::cancelled;
        } catch (const graph::AccountAuthenticationError&) {
            if (cache) {
                cache->clear();
            }
            throw;
        } catch (const std::runtime_error& failure) {
            observer.rethrow_failure();
            error = failure.what();
            if (cache) {
                cache->for_drive(target.reference).clear();
            }
        }
        if (result == exit_code::cancelled || stop_token.stop_requested()) {
            if (cache) {
                cache->clear();
            }
            return exit_code::cancelled;
        }
        if (!error.empty()) {
            observer_.report_message(
                events::MessageKind::error,
                "drive_sync_failed",
                std::format("Drive '{}': {}", target.reference, error)
            );
        }
        if (result == exit_code::failure) {
            ++failed;
            aggregate = exit_code::failure;
        } else if (result == exit_code::blocked) {
            ++blocked;
            if (aggregate == exit_code::success) {
                aggregate = exit_code::blocked;
            }
        } else {
            ++completed;
        }
        observer_.section(
            "drive_sync_result",
            "Drive synchronization result:",
            {
                {.label = "drive:",
                 .key = "drive_reference",
                 .value = target.reference},
                {.label = "status:",
                 .key = "status",
                 .value = std::to_string(result)},
            }
        );
    }
    observer_.section(
        "drive_sync_summary",
        "Multi-drive synchronization:",
        {
            {.label = "completed:",
             .key = "completed",
             .value = std::to_string(completed)},
            {.label = "failed:",
             .key = "failed",
             .value = std::to_string(failed)},
            {.label = "blocked:",
             .key = "blocked",
             .value = std::to_string(blocked)},
        }
    );
    return aggregate;
}

std::vector<graph::GraphClient*> DriveSet::graph_clients() const {
    std::vector<graph::GraphClient*> clients;
    for (const auto& target : targets_) {
        clients.push_back(target.graph.get());
    }
    return clients;
}

} // namespace onedrive::app::detail
