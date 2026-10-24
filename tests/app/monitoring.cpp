#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/app/monitoring.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <algorithm>
#include <map>
#include <stop_token>
#include <string_view>
#include <vector>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::app;

int test_multiple_drive_monitoring(
    const onedrive::events::Observer& observer, std::string_view scenario
) {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    config.drive_ids = {"first", "second"};
    config.dry_run = true;
    std::filesystem::create_directories(config.state_directory);
    std::filesystem::create_directories(config.sync_data_directory);
    std::stop_source stop;
    std::vector<std::string> synchronized;
    std::map<std::string, int> identities;
    FakeRuntimeFactory implementation;
    std::filesystem::path blocked_database;
    if (scenario == "store_retry") {
        const auto identity =
            onedrive::test::test_drive_identity("first", "first");
        const auto paths = onedrive::account::AccountState::prepare(
            config.state_directory, identity
        );
        blocked_database = paths.drive_directory / "items.sqlite3";
        std::filesystem::create_directory(blocked_database);
        implementation.use_real_item_store = true;
    }
    implementation.graph_client_override = [&](const auto& selected) {
        const auto id = selected.drive_id;
        return std::make_unique<onedrive::graph::GraphClient>(
            std::in_place_type<FakeGraphClient>,
            id,
            false,
            false,
            false,
            [&, id](std::stop_token) {
                if (++identities[id] == 1 && id == "first" &&
                    scenario == "retry") {
                    throw std::runtime_error{"temporarily unavailable Drive"};
                }
                auto identity = onedrive::test::test_drive_identity(id);
                identity.drive_id = id;
                return identity;
            },
            [&, id](const std::optional<std::string>&, std::stop_token) {
                synchronized.push_back(id);
                if (scenario == "store_retry" && id == "second" &&
                    std::ranges::count(synchronized, "second") == 1) {
                    std::filesystem::remove(blocked_database);
                }
                if (scenario == "authentication") {
                    throw onedrive::graph::AccountAuthenticationError{
                        "account consent revoked while monitoring"
                    };
                }
                if (id == "second" &&
                    std::ranges::count(synchronized, "second") == 2) {
                    stop.request_stop();
                }
                return onedrive::graph::DeltaResult{
                    .changes = {}, .delta_link = "cursor-" + id
                };
            }
        );
    };
    implementation.monitor_override =
        [](const auto& selected, auto synchronize, auto&) {
            return std::make_unique<onedrive::monitor::FileMonitor>(
                std::in_place_type<onedrive::monitor::Monitor>,
                selected.sync_data_directory,
                std::move(synchronize),
                std::chrono::milliseconds{20},
                std::chrono::milliseconds{0}
            );
        };
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, implementation
    };
    int result;
    try {
        result = onedrive::app::monitor_account(
            config, factory, observer, false, stop.get_token()
        );
        if (scenario == "authentication") {
            return fail("monitor swallowed an account authentication failure");
        }
    } catch (const onedrive::graph::AccountAuthenticationError&) {
        if (scenario != "authentication" ||
            synchronized != std::vector<std::string>{"first"} ||
            identities.contains("second")) {
            return fail("monitor continued after an account-wide failure");
        }
        return EXIT_SUCCESS;
    }
    const auto expected =
        scenario == "retry" || scenario == "store_retry"
            ? std::vector<std::string>{"second", "first", "second"}
            : std::vector<std::string>{"first", "second", "first", "second"};
    if (result != exit_code::success || synchronized != expected ||
        identities["first"] != (scenario == "retry" ? 2 : 1) ||
        identities["second"] != 1 || implementation.monitor_graph_count != 2 ||
        implementation.item_store_count !=
            (scenario == "store_retry" ? 3 : 2) ||
        implementation.metrics_count != 2 ||
        (scenario == "store_retry" &&
         implementation.item_store_sync_directories.front() !=
             implementation.item_store_sync_directories.back()) ||
        implementation.monitor_count != 1) {
        return fail(
            "monitor did not revisit every Drive sequentially with one control "
            "loop"
        );
    }
    return EXIT_SUCCESS;
}

class RecordingObserver final : public onedrive::events::Observer {
public:
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

    [[nodiscard]] bool has_message(std::string_view event) const {
        return std::ranges::any_of(events, [event](const auto& item) {
            const auto* message =
                std::get_if<onedrive::events::MessageEvent>(&item);
            return message && message->event == event;
        });
    }

    [[nodiscard]] std::vector<onedrive::events::SyncPhase> phases() const {
        std::vector<onedrive::events::SyncPhase> result;
        for (const auto& event : events) {
            if (const auto* phase =
                    std::get_if<onedrive::events::SyncPhaseEvent>(&event)) {
                result.push_back(phase->phase);
            }
        }
        return result;
    }

protected:
    void on_event(const onedrive::events::Event& event) const override {
        events.push_back(event);
    }

private:
    mutable std::vector<onedrive::events::Event> events;
};

int test_monitoring_service() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    config.dry_run = true;
    std::filesystem::create_directories(config.state_directory);

    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;
    const auto result = onedrive::app::monitor_account(
        config, runtime_factory, observer, true
    );

    if (result != exit_code::success ||
        !observer.has_message("monitor_ready") ||
        !observer.has_message("monitor_status") || observer.phases().empty() ||
        observer.phases().back() != onedrive::events::SyncPhase::waiting ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::watching,
                onedrive::events::OperationState::stopping,
                onedrive::events::OperationState::ready,
            } ||
        implementation.graph_client_count != 1 ||
        implementation.item_store_count != 1 ||
        implementation.item_store_open_count != 1 ||
        implementation.metrics_count != 1 ||
        implementation.monitor_count != 1 ||
        implementation.monitor_run_count != 1 ||
        implementation.monitor_sync_count != 1 ||
        !implementation.monitor_keyboard_exit) {
        return fail("monitoring service did not assemble and run monitoring");
    }
    return EXIT_SUCCESS;
}

int test_monitoring_service_cancellation() {
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

    const auto result = onedrive::app::monitor_account(
        config, runtime_factory, observer, false, stop.get_token()
    );
    if (result != exit_code::success || implementation.monitor_run_count != 1 ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::watching,
                onedrive::events::OperationState::stopping,
                onedrive::events::OperationState::ready,
            } ||
        implementation.monitor_sync_count != 0 ||
        !implementation.monitor_stop_requested ||
        implementation.monitor_keyboard_exit) {
        return fail("monitoring service did not forward its cancellation");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    onedrive::test::ScenarioProgress progress{6};
    RecordingObserver multi_drive_observer;
    for (const auto* scenario :
         {"success", "retry", "store_retry", "authentication"}) {
        if (progress.run(
                std::string{"multiple_drive_monitoring ("} +
                    "scenario=" + std::string{scenario} + ")",
                [&] {
                    return test_multiple_drive_monitoring(
                        multi_drive_observer, scenario
                    );
                }
            ) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
    }
    if (progress.run("monitoring_service", test_monitoring_service) !=
            EXIT_SUCCESS ||
        progress.run(
            "monitoring_service_cancellation",
            test_monitoring_service_cancellation
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return progress.finish();
}
