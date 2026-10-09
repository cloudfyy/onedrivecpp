#pragma once

#include "onedrive/http/http_options.hpp"
#include "onedrive/util/proxy_service.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <expected>
#include <memory>
#include <proxy/proxy.h>
#include <stop_token>
#include <string>
#include <utility>

namespace onedrive::monitor {

using SyncCallback = std::function<int(const std::stop_token&)>;

struct NotificationChannel {
    std::string url;
    std::chrono::steady_clock::time_point renew_at;
};

using NotificationChannelResult =
    std::expected<NotificationChannel, bool>;

struct NotificationCallbacks {
    std::function<NotificationChannelResult()> acquire_channel;
    std::function<bool()> refresh_token;
    http::ProxyOptions proxy;
    std::chrono::milliseconds request_timeout{std::chrono::seconds{60}};
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{10}};
    std::chrono::milliseconds initial_backoff{std::chrono::seconds{1}};
    std::chrono::milliseconds maximum_backoff{std::chrono::minutes{5}};

    [[nodiscard]] explicit operator bool() const noexcept {
        return acquire_channel && refresh_token;
    }
};

PRO_DEF_MEM_DISPATCH(MonitorRunDispatch, run);

struct FileMonitorFacade : pro::facade_builder
    ::add_convention<
        MonitorRunDispatch,
        int() const,
        int(bool) const,
        int(const std::stop_token&) const
    >
    ::build {};

class FileMonitor : private onedrive::util::ProxyService<FileMonitorFacade> {
    using Base = onedrive::util::ProxyService<FileMonitorFacade>;

public:
    using Base::Base;

    [[nodiscard]] int run() const {
        return implementation()->run();
    }
    [[nodiscard]] int run(bool keyboard_exit) const {
        return implementation()->run(keyboard_exit);
    }
    [[nodiscard]] int run(const std::stop_token& stop_token) const {
        return implementation()->run(stop_token);
    }
};

class Monitor final {
public:
    Monitor(
        std::filesystem::path root,
        SyncCallback synchronize,
        std::chrono::milliseconds poll_interval,
        std::chrono::milliseconds settle_delay,
        NotificationCallbacks notifications = {}
    );
    [[nodiscard]] int run() const;
    [[nodiscard]] int run(bool keyboard_exit) const;
    [[nodiscard]] int run(const std::stop_token& stop_token) const;

private:
    [[nodiscard]] int run_loop(
        const std::stop_token& stop_token,
        int signal_descriptor,
        bool keyboard_exit
    ) const;

    std::filesystem::path root_;
    SyncCallback synchronize_;
    std::chrono::milliseconds poll_interval_;
    std::chrono::milliseconds settle_delay_;
    NotificationCallbacks notifications_;
};

}  // namespace onedrive::monitor
