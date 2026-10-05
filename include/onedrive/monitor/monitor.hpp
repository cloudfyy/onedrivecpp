#pragma once

#include "onedrive/proxy_service.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <proxy/proxy.h>
#include <stop_token>
#include <utility>

namespace onedrive::monitor {

using SyncCallback = std::function<int()>;

PRO_DEF_MEM_DISPATCH(MonitorRunDispatch, run);

struct FileMonitorFacade : pro::facade_builder
    ::add_convention<MonitorRunDispatch, int() const>
    ::build {};

class FileMonitor : private detail::ProxyService<FileMonitorFacade> {
    using Base = detail::ProxyService<FileMonitorFacade>;

public:
    using Base::Base;

    [[nodiscard]] int run() const {
        return implementation()->run();
    }
};

class Monitor final {
public:
    Monitor(
        std::filesystem::path root,
        SyncCallback synchronize,
        std::chrono::milliseconds poll_interval,
        std::chrono::milliseconds settle_delay
    );
    [[nodiscard]] int run() const;
    [[nodiscard]] int run(const std::stop_token& stop_token) const;

private:
    [[nodiscard]] int run_loop(
        const std::stop_token& stop_token,
        int signal_descriptor
    ) const;

    std::filesystem::path root_;
    SyncCallback synchronize_;
    std::chrono::milliseconds poll_interval_;
    std::chrono::milliseconds settle_delay_;
};

}  // namespace onedrive::monitor
