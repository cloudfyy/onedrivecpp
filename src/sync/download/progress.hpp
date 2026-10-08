#pragma once

#include "onedrive/events/observer.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace onedrive::sync::detail {

class DownloadProgressEstimator final {
public:
    using Clock = std::chrono::steady_clock;

    explicit DownloadProgressEstimator(
        Clock::time_point started_at = Clock::now()
    );

    [[nodiscard]] events::DownloadProgressMetrics sample(
        std::uint64_t downloaded,
        std::uint64_t total,
        Clock::time_point sampled_at = Clock::now()
    );

private:
    Clock::time_point started_at_;
    Clock::time_point last_sample_at_;
    std::uint64_t last_downloaded_{0};
    std::optional<double> smoothed_bytes_per_second_;
};

class DownloadProgressReporter final {
public:
    using Clock = std::chrono::steady_clock;

    explicit DownloadProgressReporter(
        Clock::time_point started_at = Clock::now()
    );

    [[nodiscard]] bool should_report(
        std::size_t completed_files,
        std::size_t file_count,
        std::uint64_t downloaded,
        std::uint64_t total,
        util::ProgressState state,
        Clock::time_point sampled_at = Clock::now()
    );

private:
    Clock::time_point last_reported_at_;
    std::size_t last_observed_completed_files_{0};
    std::uint64_t last_observed_downloaded_{0};
    unsigned last_reported_percentage_{0};
    bool reported_{false};
};

}  // namespace onedrive::sync::detail
