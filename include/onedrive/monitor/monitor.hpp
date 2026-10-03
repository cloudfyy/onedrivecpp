#pragma once

#include <filesystem>
#include <memory>
#include <proxy/proxy.h>
#include <utility>

namespace onedrive::monitor {

PRO_DEF_MEM_DISPATCH(MonitorRunDispatch, run);

struct FileMonitorFacade : pro::facade_builder
    ::add_convention<MonitorRunDispatch, int() const>
    ::build {};

class FileMonitor {
public:
    template <typename Implementation, typename... Args>
    explicit FileMonitor(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              FileMonitorFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit FileMonitor(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit FileMonitor(Implementation& implementation)
        : implementation_{&implementation} {}

    ~FileMonitor() = default;
    FileMonitor(const FileMonitor&) = delete;
    FileMonitor& operator=(const FileMonitor&) = delete;
    FileMonitor(FileMonitor&&) noexcept = default;
    FileMonitor& operator=(FileMonitor&&) noexcept = default;

    [[nodiscard]] int run() const {
        return implementation_->run();
    }

private:
    pro::proxy<FileMonitorFacade> implementation_;
};

class Monitor final {
public:
    explicit Monitor(std::filesystem::path root);
    [[nodiscard]] int run() const;

private:
    std::filesystem::path root_;
};

}  // namespace onedrive::monitor
