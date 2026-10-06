#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>

namespace onedrive::app {

std::unique_ptr<http::HttpTransport>
ProductionRuntimeFactory::create_http_transport(
    const config::Config& config
) const {
    return std::make_unique<http::HttpTransport>(
        std::in_place_type<http::CurlHttpClient>, config.proxy
    );
}

std::unique_ptr<auth::DeviceAuthClient>
ProductionRuntimeFactory::create_device_auth_client(
    const config::Config& config, const http::HttpTransport& transport
) const {
    return std::make_unique<auth::DeviceAuthClient>(
        &transport, device_auth_options(config)
    );
}

std::unique_ptr<auth::TokenStore> ProductionRuntimeFactory::create_token_store(
    const config::Config& config
) const {
    return std::make_unique<auth::TokenStore>(
        std::in_place_type<auth::FileTokenStore>,
        account::AccountState::active_token_directory(config.state_directory)
    );
}

std::unique_ptr<graph::GraphClient>
ProductionRuntimeFactory::create_graph_client(
    const config::Config& config
) const {
    return std::make_unique<graph::GraphClient>(
        std::in_place_type<graph::MicrosoftGraphClient>,
        create_http_transport(config),
        create_token_store(config),
        device_auth_options(config),
        graph_options(config)
    );
}

std::unique_ptr<graph::GraphInfoClient>
ProductionRuntimeFactory::create_graph_info_client(
    const config::Config& config
) const {
    return std::make_unique<graph::GraphInfoClient>(
        std::in_place_type<graph::MicrosoftGraphClient>,
        create_http_transport(config),
        create_token_store(config),
        device_auth_options(config),
        graph_options(config)
    );
}

std::unique_ptr<storage::ItemStore> ProductionRuntimeFactory::create_item_store(
    const config::Config& config, const account::DriveIdentity& identity
) const {
    const auto paths =
        account::AccountState::locate(config.state_directory, identity);
    return std::make_unique<storage::ItemStore>(
        std::in_place_type<storage::ItemDatabase>,
        paths.drive_directory,
        identity
    );
}

std::unique_ptr<monitor::FileMonitor> ProductionRuntimeFactory::create_monitor(
    const config::Config& config,
    monitor::SyncCallback synchronize,
    graph::GraphClient& graph
) const {
    monitor::NotificationCallbacks notifications;
    if (config.monitor_websocket_enabled) {
        notifications = {
            .acquire_channel =
                [&graph, renewal_lead = config.monitor_websocket_renewal_lead](
                ) -> monitor::NotificationChannelResult {
                try {
                    const auto channel = graph.notification_channel();
                    const auto remaining =
                        channel.expires_at - std::chrono::system_clock::now();
                    return monitor::NotificationChannel{
                        .url = channel.notification_url,
                        .renew_at =
                            std::chrono::steady_clock::now() +
                            std::max(
                                std::chrono::system_clock::duration::zero(),
                                remaining - renewal_lead
                            ),
                    };
                } catch (const graph::NotificationChannelError& error) {
                    spdlog::warn("{}", error.what());
                    return std::unexpected{error.unauthorized()};
                } catch (const std::exception& error) {
                    spdlog::warn(
                        "Cannot acquire Graph notification channel: {}",
                        error.what()
                    );
                    return std::unexpected{false};
                }
            },
            .refresh_token =
                [&graph] {
                    try {
                        graph.refresh_access_token();
                        return true;
                    } catch (const std::exception& error) {
                        spdlog::warn(
                            "Cannot refresh notification access token: {}",
                            error.what()
                        );
                        return false;
                    }
                },
            .proxy = config.proxy,
            .request_timeout = config.monitor_websocket_request_timeout,
            .connect_timeout = config.monitor_websocket_connect_timeout,
            .initial_backoff = config.monitor_websocket_initial_backoff,
            .maximum_backoff = config.monitor_websocket_maximum_backoff,
        };
    }
    return std::make_unique<monitor::FileMonitor>(
        std::in_place_type<monitor::Monitor>,
        config.sync_data_directory,
        std::move(synchronize),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            config.monitor_poll_interval
        ),
        config.monitor_settle_delay,
        std::move(notifications)
    );
}

std::unique_ptr<metrics::Metrics>
ProductionRuntimeFactory::create_metrics(
    const config::Config& config,
    const account::DriveIdentity& identity
) const {
    const auto paths =
        account::AccountState::prepare(config.state_directory, identity);
    return std::make_unique<metrics::Metrics>(
        std::in_place_type<metrics::FileMetrics>,
        paths.drive_directory
    );
}

} // namespace onedrive::app
