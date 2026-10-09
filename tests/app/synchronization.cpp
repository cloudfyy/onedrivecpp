#include "support.hpp"

#include "onedrive/app/synchronization.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <algorithm>
#include <stop_token>
#include <vector>

namespace {

using namespace onedrive::test::app;

class RecordingObserver final : public onedrive::events::Observer {
public:
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
    }

private:
    mutable std::vector<onedrive::events::Event> events;
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
    if (result != 0 || !observer.has_delta_summary() ||
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
    if (result != 130 || implementation.graph_client_count != 1 ||
        implementation.item_store_count != 1 ||
        implementation.metrics_count != 1) {
        return fail("synchronization service did not preserve cancellation");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_synchronization_service() != EXIT_SUCCESS ||
        test_synchronization_service_cancellation() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
