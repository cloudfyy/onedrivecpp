#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/app/synchronization.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/sync/core/engine.hpp"

#include <algorithm>
#include <stop_token>
#include <vector>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::app;

class RecordingObserver final : public onedrive::events::Observer {
public:
    explicit RecordingObserver(
        std::function<void(const onedrive::events::Event&)> callback = {}
    )
        : callback_{std::move(callback)} {
    }
    [[nodiscard]] std::vector<onedrive::events::OperationState>
    operation_states() const {
        std::vector<onedrive::events::OperationState> states;
        for (const auto& event : events) {
            if (const auto* state =
                    std::get_if<onedrive::events::OperationStateEvent>(
                        &event
                    )) {
                states.push_back(state->state);
            }
        }
        return states;
    }

    [[nodiscard]] bool has_delta_summary() const {
        return std::ranges::any_of(events, [](const auto& event) {
            return std::holds_alternative<
                onedrive::events::DeltaSummaryEvent
            >(event);
        });
    }

protected:
    void on_event(const onedrive::events::Event& event) const override {
        events.push_back(event);
        if (callback_) {
            callback_(event);
        }
    }

private:
    mutable std::vector<onedrive::events::Event> events;
    std::function<void(const onedrive::events::Event&)> callback_;
};

int test_synchronization_service() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    config.dry_run = true;
    std::filesystem::create_directories(config.state_directory);
    std::filesystem::create_directories(config.sync_data_directory);

    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;

    const auto result = onedrive::app::synchronize_account(
        config, runtime_factory, observer
    );
    const auto identity = onedrive::test::test_drive_identity("me");
    const auto expected_sync_directory =
        onedrive::account::AccountState::drive_data_directory(
            config.sync_data_directory, identity
        );
    if (result != exit_code::success || !observer.has_delta_summary() ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::ready,
            } ||
        implementation.graph_client_count != 1 ||
        implementation.item_store_count != 1 ||
        implementation.item_store_open_count != 1 ||
        implementation.metrics_count != 1 ||
        implementation.last_item_store_sync_directory !=
            expected_sync_directory) {
        return fail(
            "synchronization service did not prepare and run the sync "
            "through its injected dependencies"
        );
    }
    return EXIT_SUCCESS;
}

int test_synchronization_service_cancellation() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";

    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;
    std::stop_source stop;
    stop.request_stop();

    const auto result = onedrive::app::synchronize_account(
        config, runtime_factory, observer, stop.get_token()
    );
    if (result != exit_code::cancelled ||
        implementation.graph_client_count != 0 ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::stopping,
                onedrive::events::OperationState::ready,
            } ||
        implementation.item_store_count != 0 ||
        implementation.metrics_count != 0) {
        return fail("synchronization service did not preserve cancellation");
    }
    return EXIT_SUCCESS;
}

int test_identity_cancellation() {
    for (const bool throws : {false, true}) {
        auto config = onedrive::config::Config::defaults();
        FakeRuntimeFactory implementation;
        std::stop_source stop;
        bool forwarded = false;
        implementation.identity_override = [&](std::stop_token token) {
            forwarded = token == stop.get_token();
            stop.request_stop();
            if (throws) {
                throw onedrive::graph::RequestCancelledError{
                    "identity cancelled"
                };
            }
            return onedrive::test::test_drive_identity("me");
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, implementation
        };
        RecordingObserver observer;
        if (onedrive::app::synchronize_account(
                config, factory, observer, stop.get_token()
            ) != exit_code::cancelled ||
            !forwarded || implementation.graph_client_count != 1 ||
            implementation.item_store_count != 0 ||
            implementation.metrics_count != 0 ||
            observer.operation_states() !=
                std::vector<onedrive::events::OperationState>{
                    onedrive::events::OperationState::syncing,
                    onedrive::events::OperationState::stopping,
                    onedrive::events::OperationState::ready,
                }) {
            return fail(
                "identity cancellation created stores or failed to return "
                "cancelled"
            );
        }
    }
    FakeRuntimeFactory implementation;
    implementation.identity_override =
        [](std::stop_token) -> onedrive::account::DriveIdentity {
        throw std::runtime_error{"identity failed"};
    };
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::synchronize_account(
                    onedrive::config::Config::defaults(), factory, observer
                ));
            },
            "identity failed"
        ) ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::failed,
            }) {
        return fail("identity failure was hidden or treated as cancellation");
    }
    return EXIT_SUCCESS;
}

int test_populated_cache_cleanup() {
    namespace od = onedrive;
    using State = od::events::OperationState;
    for (const int failure_stage : {0, 1, 2, 3, 4, 5}) {
        TemporaryDirectory temporary;
        auto config = od::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.dry_run = true;
        FakeRuntimeFactory implementation;
        const od::app::RuntimeFactory factory{
            od::util::borrowed_proxy, implementation
        };
        od::sync::PreviewCache cache;
        RecordingObserver seed_observer;
        if (od::app::synchronize_account(
                config, factory, seed_observer, {}, &cache
            ) != exit_code::success ||
            !cache.available()) {
            return fail("cannot seed application preview cache");
        }
        const auto graph_count = implementation.graph_client_count;
        const auto store_count = implementation.item_store_count;
        const auto metrics_count = implementation.metrics_count;
        std::stop_source stop;
        if (failure_stage == 0) {
            stop.request_stop();
        } else if (failure_stage == 1 || failure_stage == 2) {
            implementation.identity_override = [&](std::stop_token token) {
                if (token != stop.get_token()) {
                    throw std::logic_error{
                        "identity did not receive caller stop token"
                    };
                }
                stop.request_stop();
                if (failure_stage == 2) {
                    throw od::graph::RequestCancelledError{
                        "identity cancelled"
                    };
                }
                return od::test::test_drive_identity("me");
            };
        } else if (failure_stage == 3) {
            implementation.identity_override =
                [](std::stop_token) -> od::account::DriveIdentity {
                throw std::runtime_error{
                    "identity failed with populated cache"
                };
            };
        } else if (failure_stage == 4) {
            implementation.delta_override = [&](const auto&, std::stop_token) {
                stop.request_stop();
                return od::graph::DeltaResult{};
            };
        }
        RecordingObserver observer{[&](const od::events::Event& event) {
            const auto* state =
                std::get_if<od::events::OperationStateEvent>(&event);
            if (failure_stage == 5 && state && state->state == State::syncing) {
                throw std::runtime_error{"startup observer failed"};
            }
        }};
        const bool failure = failure_stage == 3 || failure_stage == 5;
        if (failure) {
            if (!od::test::throws_with<std::runtime_error>(
                    [&] {
                        static_cast<void>(od::app::synchronize_account(
                            config, factory, observer, stop.get_token(), &cache
                        ));
                    },
                    failure_stage == 3 ? "identity failed with populated cache"
                                       : "startup observer failed"
                ) ||
                observer.operation_states() !=
                    (failure_stage == 5
                         ? std::vector<State>{State::syncing}
                         : std::vector<State>{State::syncing, State::failed})) {
                return fail(
                    "application cache failure was hidden or state was not "
                    "failed"
                );
            }
        } else if (od::app::synchronize_account(
                       config, factory, observer, stop.get_token(), &cache
                   ) != exit_code::cancelled ||
                   observer.operation_states() !=
                       std::vector<State>{
                           State::syncing, State::stopping, State::ready
                       }) {
            return fail(
                "application cache cancellation changed lifecycle semantics"
            );
        }
        if (cache.available() ||
            implementation.graph_client_count !=
                graph_count +
                    (failure_stage == 0 || failure_stage == 5 ? 0 : 1) ||
            implementation.item_store_count !=
                store_count + (failure_stage == 4 ? 1 : 0) ||
            implementation.metrics_count !=
                metrics_count + (failure_stage == 4 ? 1 : 0)) {
            return fail(
                "early cancellation/failure retained a cache or created "
                "unexpected dependencies"
            );
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_synchronization_service() != EXIT_SUCCESS ||
        test_synchronization_service_cancellation() != EXIT_SUCCESS ||
        test_identity_cancellation() != EXIT_SUCCESS ||
        test_populated_cache_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
