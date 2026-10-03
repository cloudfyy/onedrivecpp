#include "onedrive/app/runtime_factory.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"

#include <memory>

namespace onedrive::app {

std::unique_ptr<http::HttpTransport>
ProductionRuntimeFactory::create_http_transport() const {
    return std::make_unique<http::HttpTransport>(
        std::in_place_type<http::CurlHttpClient>
    );
}

std::unique_ptr<auth::DeviceAuthClient>
ProductionRuntimeFactory::create_device_auth_client(
    const config::Config& config,
    const http::HttpTransport& transport
) const {
    return std::make_unique<auth::DeviceAuthClient>(
        &transport,
        auth::DeviceAuthOptions{
            .application_id = config.application_id,
            .tenant_id = config.azure_tenant_id,
            .auth_endpoint = config.auth_endpoint,
            .scope = config.auth_scope,
        }
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

std::unique_ptr<graph::GraphClient> ProductionRuntimeFactory::create_graph_client(
    const config::Config& config
) const {
    return std::make_unique<graph::GraphClient>(
        std::in_place_type<graph::MicrosoftGraphClient>,
        create_http_transport(),
        create_token_store(config),
        auth::DeviceAuthOptions{
            .application_id = config.application_id,
            .tenant_id = config.azure_tenant_id,
            .auth_endpoint = config.auth_endpoint,
            .scope = config.auth_scope,
        },
        graph::GraphOptions{
            .drive_id = config.drive_id,
            .endpoint = "https://graph.microsoft.com/v1.0",
            .maximum_throttle_retries =
                config.graph_maximum_throttle_retries,
            .initial_throttle_delay = config.graph_initial_throttle_delay,
            .maximum_throttle_delay = config.graph_maximum_throttle_delay,
            .download_chunk_threshold_bytes =
                config.download_chunk_threshold_bytes,
        }
    );
}

std::unique_ptr<storage::ItemStore> ProductionRuntimeFactory::create_item_store(
    const config::Config& config,
    const account::DriveIdentity& identity
) const {
    const auto paths =
        account::AccountState::prepare(config.state_directory, identity);
    return std::make_unique<storage::ItemStore>(
        std::in_place_type<storage::ItemDatabase>,
        paths.drive_directory,
        identity
    );
}

std::unique_ptr<monitor::FileMonitor> ProductionRuntimeFactory::create_monitor(
    const config::Config& config
) const {
    return std::make_unique<monitor::FileMonitor>(
        std::in_place_type<monitor::Monitor>,
        config.sync_directory
    );
}

std::unique_ptr<metrics::Metrics> ProductionRuntimeFactory::create_metrics() const {
    return std::make_unique<metrics::Metrics>(
        std::in_place_type<metrics::NullMetrics>
    );
}

}  // namespace onedrive::app
