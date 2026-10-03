#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"

#include <memory>
#include <proxy/proxy.h>
#include <utility>

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

PRO_DEF_MEM_DISPATCH(FactoryHttpDispatch, create_http_transport);
PRO_DEF_MEM_DISPATCH(FactoryAuthDispatch, create_device_auth_client);
PRO_DEF_MEM_DISPATCH(FactoryTokenDispatch, create_token_store);
PRO_DEF_MEM_DISPATCH(FactoryGraphDispatch, create_graph_client);
PRO_DEF_MEM_DISPATCH(FactoryStoreDispatch, create_item_store);
PRO_DEF_MEM_DISPATCH(FactoryMonitorDispatch, create_monitor);
PRO_DEF_MEM_DISPATCH(FactoryMetricsDispatch, create_metrics);

struct RuntimeFactoryFacade : pro::facade_builder
    ::add_convention<
        FactoryHttpDispatch,
        std::unique_ptr<http::HttpTransport>() const
    >
    ::add_convention<
        FactoryAuthDispatch,
        std::unique_ptr<auth::DeviceAuthClient>(
            const config::Config&,
            const http::HttpTransport&
        ) const
    >
    ::add_convention<
        FactoryTokenDispatch,
        std::unique_ptr<auth::TokenStore>(const config::Config&) const
    >
    ::add_convention<
        FactoryGraphDispatch,
        std::unique_ptr<graph::GraphClient>(const config::Config&) const
    >
    ::add_convention<
        FactoryStoreDispatch,
        std::unique_ptr<storage::ItemStore>(
            const config::Config&,
            const account::DriveIdentity&
        ) const
    >
    ::add_convention<
        FactoryMonitorDispatch,
        std::unique_ptr<monitor::FileMonitor>(const config::Config&) const
    >
    ::add_convention<
        FactoryMetricsDispatch,
        std::unique_ptr<metrics::Metrics>() const
    >
    ::build {};

class RuntimeFactory {
public:
    template <typename Implementation, typename... Args>
    explicit RuntimeFactory(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              RuntimeFactoryFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit RuntimeFactory(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit RuntimeFactory(Implementation& implementation)
        : implementation_{&implementation} {}

    ~RuntimeFactory() = default;
    RuntimeFactory(const RuntimeFactory&) = delete;
    RuntimeFactory& operator=(const RuntimeFactory&) = delete;
    RuntimeFactory(RuntimeFactory&&) noexcept = default;
    RuntimeFactory& operator=(RuntimeFactory&&) noexcept = default;

    [[nodiscard]] std::unique_ptr<http::HttpTransport>
    create_http_transport() const {
        return implementation_->create_http_transport();
    }

    [[nodiscard]] std::unique_ptr<auth::DeviceAuthClient>
    create_device_auth_client(
        const config::Config& config,
        const http::HttpTransport& transport
    ) const {
        return implementation_->create_device_auth_client(config, transport);
    }

    [[nodiscard]] std::unique_ptr<auth::TokenStore> create_token_store(
        const config::Config& config
    ) const {
        return implementation_->create_token_store(config);
    }

    [[nodiscard]] std::unique_ptr<graph::GraphClient> create_graph_client(
        const config::Config& config
    ) const {
        return implementation_->create_graph_client(config);
    }

    [[nodiscard]] std::unique_ptr<storage::ItemStore> create_item_store(
        const config::Config& config,
        const account::DriveIdentity& identity
    ) const {
        return implementation_->create_item_store(config, identity);
    }

    [[nodiscard]] std::unique_ptr<monitor::FileMonitor> create_monitor(
        const config::Config& config
    ) const {
        return implementation_->create_monitor(config);
    }

    [[nodiscard]] std::unique_ptr<metrics::Metrics> create_metrics() const {
        return implementation_->create_metrics();
    }

private:
    pro::proxy<RuntimeFactoryFacade> implementation_;
};

class ProductionRuntimeFactory final {
public:
    [[nodiscard]] std::unique_ptr<http::HttpTransport>
    create_http_transport() const;
    [[nodiscard]] std::unique_ptr<auth::DeviceAuthClient>
    create_device_auth_client(
        const config::Config& config,
        const http::HttpTransport& transport
    ) const;
    [[nodiscard]] std::unique_ptr<auth::TokenStore> create_token_store(
        const config::Config& config
    ) const;
    [[nodiscard]] std::unique_ptr<graph::GraphClient> create_graph_client(
        const config::Config& config
    ) const;
    [[nodiscard]] std::unique_ptr<storage::ItemStore> create_item_store(
        const config::Config& config,
        const account::DriveIdentity& identity
    ) const;
    [[nodiscard]] std::unique_ptr<monitor::FileMonitor> create_monitor(
        const config::Config& config
    ) const;
    [[nodiscard]] std::unique_ptr<metrics::Metrics> create_metrics() const;
};

}  // namespace onedrive::app
