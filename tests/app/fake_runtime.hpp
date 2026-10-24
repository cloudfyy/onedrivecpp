#pragma once

#include "fake_auth.hpp"
#include "fake_graph.hpp"
#include "fake_items.hpp"
#include "onedrive/account/account_state.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::test::app {

class FakeMonitor final {
public:
    FakeMonitor(
        int& run_count,
        bool& keyboard_exit,
        bool& stop_requested,
        onedrive::monitor::SyncCallback synchronize
    )
        : run_count_{run_count},
          keyboard_exit_{keyboard_exit},
          stop_requested_{stop_requested},
          synchronize_{std::move(synchronize)} {
    }

    [[nodiscard]] int run() const {
        ++run_count_;
        static_cast<void>(synchronize_({}));
        return exit_code::success;
    }
    [[nodiscard]] int run(bool) const {
        return run();
    }
    [[nodiscard]] int run(const std::stop_token& stop_token) const {
        ++run_count_;
        return synchronize_(stop_token);
    }
    [[nodiscard]] int
    run(bool keyboard_exit, const std::stop_token& stop_token) const {
        keyboard_exit_ = keyboard_exit;
        stop_requested_ = stop_token.stop_requested();
        ++run_count_;
        if (!stop_requested_) {
            static_cast<void>(synchronize_(stop_token));
        }
        return exit_code::success;
    }

private:
    int& run_count_;
    bool& keyboard_exit_;
    bool& stop_requested_;
    onedrive::monitor::SyncCallback synchronize_;
};

class FakeMetrics final {
public:
    void record_sync_run(
        onedrive::metrics::SyncRunOutcome, std::chrono::duration<double>
    ) noexcept {
    }
};

class FakeRuntimeFactory final {
public:
    [[nodiscard]] std::unique_ptr<onedrive::http::HttpTransport>
    create_http_transport(const onedrive::config::Config&) const {
        return std::make_unique<onedrive::http::HttpTransport>(
            std::in_place_type<FakeAuthenticationTransport>,
            authentication_response_override
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::DeviceAuthClient>
    create_device_auth_client(
        const onedrive::config::Config& config,
        const onedrive::http::HttpTransport& transport
    ) const {
        return std::make_unique<onedrive::auth::DeviceAuthClient>(
            &transport,
            onedrive::app::device_auth_options(config),
            [](std::chrono::seconds) {}
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::TokenStore>
    create_token_store(const onedrive::config::Config& config) const {
        ++token_store_count;
        return std::make_unique<onedrive::auth::TokenStore>(
            std::in_place_type<FakeTokenStore>,
            onedrive::account::AccountState::active_token_directory(
                config.state_directory
            ) / "refresh_token"
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::graph::GraphClient>
    create_graph_client(const onedrive::config::Config& config) const {
        ++graph_client_count;
        if (graph_client_override) {
            return graph_client_override(config);
        }
        return std::make_unique<onedrive::graph::GraphClient>(
            std::in_place_type<FakeGraphClient>,
            configured_drive_id,
            false,
            false,
            false,
            identity_override,
            delta_override
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::graph::GraphInfoClient>
    create_graph_info_client(const onedrive::config::Config& config) const {
        ++graph_info_client_count;
        if (graph_info_override) {
            return graph_info_override(config);
        }
        return std::make_unique<onedrive::graph::GraphInfoClient>(
            std::in_place_type<FakeGraphClient>,
            configured_drive_id,
            empty_shared,
            empty_sites,
            include_configured_drive
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::storage::ItemStore>
    create_item_store(
        const onedrive::config::Config& config,
        const onedrive::account::DriveIdentity& identity
    ) const {
        ++item_store_count;
        item_store_sync_directories.push_back(config.sync_data_directory);
        last_item_store_sync_directory = config.sync_data_directory;
        if (use_real_item_store) {
            return std::make_unique<onedrive::storage::ItemStore>(
                std::in_place_type<onedrive::storage::ItemDatabase>,
                onedrive::account::AccountState::locate(
                    config.state_directory, identity
                )
                    .drive_directory,
                identity
            );
        }
        return std::make_unique<onedrive::storage::ItemStore>(
            std::in_place_type<FakeItemStore>,
            item_store_open_count,
            item_store_apply_delta_count,
            item_store_reset_count,
            reset_drive_id,
            clear_count_,
            clear_drive_id_,
            item_states,
            partial_download_states,
            pending_download_states,
            blocked_item_states
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::monitor::FileMonitor>
    create_monitor(
        const onedrive::config::Config& config,
        onedrive::monitor::SyncCallback synchronize,
        std::span<onedrive::graph::GraphClient* const> graphs
    ) const {
        ++monitor_count;
        monitor_graph_count = graphs.size();
        auto counted_synchronize = [this, synchronize = std::move(synchronize)](
                                       const std::stop_token& stop_token
                                   ) {
            ++monitor_sync_count;
            return synchronize(stop_token);
        };
        if (monitor_override) {
            return monitor_override(
                config, std::move(counted_synchronize), *graphs.front()
            );
        }
        return std::make_unique<onedrive::monitor::FileMonitor>(
            std::in_place_type<FakeMonitor>,
            monitor_run_count,
            monitor_keyboard_exit,
            monitor_stop_requested,
            std::move(counted_synchronize)
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::metrics::Metrics> create_metrics(
        const onedrive::config::Config&, const onedrive::account::DriveIdentity&
    ) const {
        ++metrics_count;
        return std::make_unique<onedrive::metrics::Metrics>(
            std::in_place_type<FakeMetrics>
        );
    }

    std::string configured_drive_id{"me"};
    std::function<std::unique_ptr<onedrive::graph::GraphClient>(
        const onedrive::config::Config&
    )>
        graph_client_override;
    std::function<std::unique_ptr<onedrive::graph::GraphInfoClient>(
        const onedrive::config::Config&
    )>
        graph_info_override;
    mutable std::vector<std::filesystem::path> item_store_sync_directories;
    mutable std::size_t monitor_graph_count{0};
    FakeGraphClient::IdentityOverride identity_override;
    FakeGraphClient::DeltaOverride delta_override;
    std::function<std::unique_ptr<onedrive::monitor::FileMonitor>(
        const onedrive::config::Config&,
        onedrive::monitor::SyncCallback,
        onedrive::graph::GraphClient&
    )>
        monitor_override;
    FakeAuthenticationTransport::ResponseOverride
        authentication_response_override;
    bool empty_shared{false};
    bool empty_sites{false};
    bool include_configured_drive{false};
    bool use_real_item_store{false};
    mutable int token_store_count{0};
    mutable int graph_client_count{0};
    mutable int graph_info_client_count{0};
    mutable int item_store_count{0};
    mutable int item_store_open_count{0};
    mutable int item_store_apply_delta_count{0};
    mutable int item_store_reset_count{0};
    mutable std::string reset_drive_id;
    mutable int clear_count_{0};
    mutable std::string clear_drive_id_;
    mutable int monitor_count{0};
    mutable int monitor_run_count{0};
    mutable int monitor_sync_count{0};
    mutable bool monitor_keyboard_exit{false};
    mutable bool monitor_stop_requested{false};
    mutable int metrics_count{0};
    mutable std::filesystem::path last_item_store_sync_directory;
    std::vector<onedrive::storage::ItemState> item_states;
    std::vector<onedrive::storage::PartialDownload> partial_download_states;
    std::vector<onedrive::storage::PendingDownload> pending_download_states;
    mutable std::vector<onedrive::storage::BlockedItem> blocked_item_states;
};

} // namespace onedrive::test::app
