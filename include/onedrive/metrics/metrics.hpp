#pragma once

#include <chrono>
#include <memory>
#include <proxy/proxy.h>
#include <utility>

namespace onedrive::metrics {

PRO_DEF_MEM_DISPATCH(RecordSyncRunDispatch, record_sync_run);

struct MetricsFacade : pro::facade_builder
    ::add_convention<
        RecordSyncRunDispatch,
        void(bool, std::chrono::duration<double>) noexcept
    >
    ::build {};

class Metrics {
public:
    template <typename Implementation, typename... Args>
    explicit Metrics(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              MetricsFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit Metrics(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit Metrics(Implementation& implementation)
        : implementation_{&implementation} {}

    ~Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics(Metrics&&) noexcept = default;
    Metrics& operator=(Metrics&&) noexcept = default;

    void record_sync_run(
        bool success,
        std::chrono::duration<double> duration
    ) noexcept {
        implementation_->record_sync_run(success, duration);
    }

private:
    pro::proxy<MetricsFacade> implementation_;
};

class NullMetrics final {
public:
    void record_sync_run(
        bool success,
        std::chrono::duration<double> duration
    ) noexcept;
};

}  // namespace onedrive::metrics
