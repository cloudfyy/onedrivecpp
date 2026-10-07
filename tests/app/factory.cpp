#include "support.hpp"

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

int test_production_factory() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.application_id = "test-application";
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    const onedrive::account::DriveIdentity identity{
        .user_id = "user-id",
        .user_display_name = "Test User",
        .configured_drive_id = "me",
        .drive_id = "drive-id",
        .drive_name = "Test Drive",
    };
    std::filesystem::create_directories(config.state_directory);
    [[maybe_unused]] const auto account_paths =
        onedrive::account::AccountState::activate(
            config.state_directory, identity, "refresh-token"
        );

    onedrive::app::ProductionRuntimeFactory factory;
    auto transport = factory.create_http_transport(config);
    auto authentication = factory.create_device_auth_client(config, *transport);
    auto tokens = factory.create_token_store(config);
    auto graph = factory.create_graph_client(config);
    auto graph_info = factory.create_graph_info_client(config);
    auto items = factory.create_item_store(config, identity);
    auto metrics = factory.create_metrics(config, identity);

    config.monitor_websocket_enabled = false;
    auto polling_monitor =
        factory.create_monitor(config, [] { return 0; }, *graph);
    config.monitor_websocket_enabled = true;
    auto notification_monitor =
        factory.create_monitor(config, [] { return 0; }, *graph);

    if (!transport || !authentication || !tokens || !graph || !graph_info ||
        !items || !metrics || !polling_monitor || !notification_monitor) {
        return fail("production runtime factory returned an empty component");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_production_factory();
}
