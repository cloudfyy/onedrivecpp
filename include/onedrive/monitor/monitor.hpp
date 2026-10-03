#pragma once

#include "onedrive/proxy_service.hpp"

#include <filesystem>
#include <memory>
#include <proxy/proxy.h>
#include <utility>

namespace onedrive::monitor {

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
    explicit Monitor(std::filesystem::path root);
    [[nodiscard]] int run() const;

private:
    std::filesystem::path root_;
};

}  // namespace onedrive::monitor
