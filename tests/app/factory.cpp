#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "graph/support.hpp"

#include <atomic>
#include <string_view>
#include <thread>

namespace {

namespace exit_code = onedrive::exit_code;

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;
using namespace std::chrono_literals;

int test_production_notification_fallback() {
    namespace graph_test = onedrive::test::graph;
    struct Case {
        std::string_view setup;
        std::size_t expected_requests;
    };
    for (const auto& [setup, expected_requests] :
         {Case{"unavailable", 2},
          Case{"transient", 3},
          Case{"no-expiration", 3},
          Case{"with-expiration", 3},
          Case{"unauthorized", 4},
          Case{"refresh-failure", 5}}) {
        TemporaryDirectory temporary;
        auto config = onedrive::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.monitor_poll_interval = 1s;
        config.monitor_settle_delay = 1ms;
        std::filesystem::create_directory(config.state_directory);
        std::filesystem::permissions(
            config.state_directory, std::filesystem::perms::owner_all
        );
        std::filesystem::create_directory(config.sync_data_directory);
        std::deque<onedrive::http::HttpResult> responses{
            graph_test::json_response(
                200,
                R"({"expires_in":3600,"access_token":"access","refresh_token":"refresh"})"
            ),
        };
        if (setup == "transient") {
            responses.push_back(
                graph_test::json_response(503, "temporary-response-secret")
            );
        } else if (setup == "no-expiration") {
            responses.push_back(
                graph_test::json_response(
                    200,
                    R"({"notificationUrl":"http://invalid.example.test/endpoint-secret"})"
                )
            );
        } else if (setup == "with-expiration") {
            responses.push_back(
                graph_test::json_response(
                    200,
                    R"({"notificationUrl":"http://invalid.example.test/endpoint-secret","expirationDateTime":"2040-01-01T00:00:00Z"})"
                )
            );
        } else if (setup == "unauthorized" || setup == "refresh-failure") {
            responses.push_back(
                graph_test::json_response(401, "authentication-response-secret")
            );
            if (setup == "refresh-failure") {
                responses.push_back(
                    graph_test::json_response(
                        400,
                        R"({"error":"invalid_grant","error_description":"grant revoked"})"
                    )
                );
            }
            responses.push_back(
                graph_test::json_response(
                    200,
                    R"({"expires_in":3600,"access_token":"renewed-access","refresh_token":"refresh"})"
                )
            );
        }
        responses.push_back(
            graph_test::json_response(404, "unsupported-response-secret")
        );
        auto transport =
            std::make_unique<graph_test::FakeTransport>(std::move(responses));
        auto* requests = transport.get();
        onedrive::graph::GraphClient graph{
            std::in_place_type<onedrive::graph::MicrosoftGraphClient>,
            graph_test::wrap_transport(std::move(transport)),
            graph_test::wrap_token_store(
                std::make_unique<graph_test::FakeTokenStore>("refresh")
            ),
            graph_test::auth_options(),
        };
        onedrive::app::ProductionRuntimeFactory factory;
        std::atomic_int rounds{0};
        auto monitor = factory.create_monitor(
            config,
            [&](const std::stop_token&) {
                ++rounds;
                return exit_code::success;
            },
            graph
        );
        std::atomic_int result{-1};
        std::jthread worker{[&](std::stop_token token) {
            result = monitor->run(false, token);
        }};
        const bool ready =
            onedrive::test::wait_until([&] { return rounds >= 4; }, 5s, 5ms);
        worker.request_stop();
        worker.join();
        if (!ready || result != exit_code::success ||
            requests->queued.requests.size() != expected_requests) {
            return fail(
                "production Monitor did not retry transient setup then retain "
                "polling without unsupported-channel retries"
            );
        }
    }
    return EXIT_SUCCESS;
}

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
    auto polling_monitor = factory.create_monitor(
        config,
        [](const std::stop_token&) { return exit_code::success; },
        *graph
    );
    config.monitor_websocket_enabled = true;
    auto notification_monitor = factory.create_monitor(
        config,
        [](const std::stop_token&) { return exit_code::success; },
        *graph
    );

    if (!transport || !authentication || !tokens || !graph || !graph_info ||
        !items || !metrics || !polling_monitor || !notification_monitor) {
        return fail("production runtime factory returned an empty component");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{2};
    if (progress.run(
            "production_notification_fallback",
            test_production_notification_fallback
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return progress.finish(
        progress.run("production_factory", test_production_factory)
    );
}
