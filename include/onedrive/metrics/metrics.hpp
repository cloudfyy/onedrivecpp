#pragma once

#include "onedrive/util/proxy_service.hpp"

#include <chrono>
#include <memory>
#include <proxy/proxy.h>
#include <utility>

namespace onedrive::metrics {

enum class SyncRunOutcome {
    succeeded,
    failed,
};

PRO_DEF_MEM_DISPATCH(RecordSyncRunDispatch, record_sync_run);

struct MetricsFacade : pro::facade_builder
    ::add_convention<
        RecordSyncRunDispatch,
        void(SyncRunOutcome, std::chrono::duration<double>) noexcept
    >
    ::build {};

class Metrics : private onedrive::util::ProxyService<MetricsFacade> {
    using Base = onedrive::util::ProxyService<MetricsFacade>;

public:
    using Base::Base;

    void record_sync_run(
        SyncRunOutcome outcome,
        std::chrono::duration<double> duration
    ) noexcept {
        implementation()->record_sync_run(outcome, duration);
    }
};

class NullMetrics final {
public:
    void record_sync_run(
        SyncRunOutcome outcome,
        std::chrono::duration<double> duration
    ) noexcept;
};

}  // namespace onedrive::metrics
