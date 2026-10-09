#include "support.hpp"

#include "onedrive/app/monitoring.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <algorithm>
#include <stop_token>
#include <string_view>
#include <vector>

namespace {

using namespace onedrive::test::app;

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

    if (result != 0 || !observer.has_message("monitor_ready") ||
        !observer.has_message("monitor_status") ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::watching,
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
    if (result != 0 || implementation.monitor_run_count != 1 ||
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
    if (test_monitoring_service() != EXIT_SUCCESS ||
        test_monitoring_service_cancellation() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
