#include "onedrive/app/runtime_factory.hpp"

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
    return std::make_unique<http::CurlHttpClient>();
}

std::unique_ptr<auth::DeviceAuthClient>
ProductionRuntimeFactory::create_device_auth_client(
    const config::Config& config,
    const http::HttpTransport& transport
) const {
    return std::make_unique<auth::DeviceAuthClient>(
        transport,
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
    return std::make_unique<auth::FileTokenStore>(config.state_directory);
}

std::unique_ptr<graph::GraphClient> ProductionRuntimeFactory::create_graph_client(
    const config::Config&
) const {
    return std::make_unique<graph::MicrosoftGraphClient>();
}

std::unique_ptr<storage::ItemStore> ProductionRuntimeFactory::create_item_store(
    const config::Config& config
) const {
    return std::make_unique<storage::ItemDatabase>(config.state_directory);
}

std::unique_ptr<monitor::FileMonitor> ProductionRuntimeFactory::create_monitor(
    const config::Config& config
) const {
    return std::make_unique<monitor::Monitor>(config.sync_directory);
}

std::unique_ptr<metrics::Metrics> ProductionRuntimeFactory::create_metrics() const {
    return std::make_unique<metrics::NullMetrics>();
}

}  // namespace onedrive::app
