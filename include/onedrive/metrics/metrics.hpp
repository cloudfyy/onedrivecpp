#pragma once

#include "onedrive/util/proxy_service.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <proxy/proxy.h>
#include <string>
#include <utility>

namespace onedrive::metrics {

enum class SyncRunOutcome {
    succeeded,
    failed,
};

struct SyncRunStatus {
    SyncRunOutcome outcome{SyncRunOutcome::failed};
    std::int64_t completed_at_unix_seconds{0};
    std::uint64_t duration_milliseconds{0};
};

[[nodiscard]] std::optional<SyncRunStatus>
load_sync_run_status(const std::filesystem::path& state_directory);

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

class FileMetrics final {
public:
    explicit FileMetrics(std::filesystem::path state_directory);

    void record_sync_run(
        SyncRunOutcome outcome,
        std::chrono::duration<double> duration
    ) noexcept;

private:
    std::filesystem::path state_directory_;
};

}  // namespace onedrive::metrics
