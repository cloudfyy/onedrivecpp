#pragma once

#include <chrono>

namespace onedrive::metrics {

class Metrics {
public:
    virtual ~Metrics() = default;

    virtual void record_sync_run(
        bool success,
        std::chrono::duration<double> duration
    ) noexcept = 0;
};

class NullMetrics final : public Metrics {
public:
    void record_sync_run(
        bool success,
        std::chrono::duration<double> duration
    ) noexcept override;
};

}  // namespace onedrive::metrics
