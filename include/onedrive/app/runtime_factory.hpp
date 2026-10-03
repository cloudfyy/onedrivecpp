#pragma once

#include <memory>

namespace onedrive::auth {
class DeviceAuthClient;
class TokenStore;
}

namespace onedrive::account {
struct DriveIdentity;
}

namespace onedrive::config {
struct Config;
}

namespace onedrive::graph {
class GraphClient;
}

namespace onedrive::http {
class HttpTransport;
}

namespace onedrive::metrics {
class Metrics;
}

namespace onedrive::monitor {
class FileMonitor;
}

namespace onedrive::storage {
class ItemStore;
}

namespace onedrive::app {

class RuntimeFactory {
public:
    virtual ~RuntimeFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<http::HttpTransport>
    create_http_transport() const = 0;
    [[nodiscard]] virtual std::unique_ptr<auth::DeviceAuthClient>
    create_device_auth_client(
        const config::Config& config,
        const http::HttpTransport& transport
    ) const = 0;
    [[nodiscard]] virtual std::unique_ptr<auth::TokenStore> create_token_store(
        const config::Config& config
    ) const = 0;
    [[nodiscard]] virtual std::unique_ptr<graph::GraphClient> create_graph_client(
        const config::Config& config
    ) const = 0;
    [[nodiscard]] virtual std::unique_ptr<storage::ItemStore> create_item_store(
        const config::Config& config,
        const account::DriveIdentity& identity
    ) const = 0;
    [[nodiscard]] virtual std::unique_ptr<monitor::FileMonitor> create_monitor(
        const config::Config& config
    ) const = 0;
    [[nodiscard]] virtual std::unique_ptr<metrics::Metrics> create_metrics() const = 0;
};

class ProductionRuntimeFactory final : public RuntimeFactory {
public:
    [[nodiscard]] std::unique_ptr<http::HttpTransport>
    create_http_transport() const override;
    [[nodiscard]] std::unique_ptr<auth::DeviceAuthClient>
    create_device_auth_client(
        const config::Config& config,
        const http::HttpTransport& transport
    ) const override;
    [[nodiscard]] std::unique_ptr<auth::TokenStore> create_token_store(
        const config::Config& config
    ) const override;
    [[nodiscard]] std::unique_ptr<graph::GraphClient> create_graph_client(
        const config::Config& config
    ) const override;
    [[nodiscard]] std::unique_ptr<storage::ItemStore> create_item_store(
        const config::Config& config,
        const account::DriveIdentity& identity
    ) const override;
    [[nodiscard]] std::unique_ptr<monitor::FileMonitor> create_monitor(
        const config::Config& config
    ) const override;
    [[nodiscard]] std::unique_ptr<metrics::Metrics> create_metrics() const override;
};

}  // namespace onedrive::app
