#pragma once

#include <chrono>

namespace onedrive::metrics {

class Metrics {
public:
    Metrics() = default;
    virtual ~Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics(Metrics&&) = delete;
    Metrics& operator=(Metrics&&) = delete;

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
