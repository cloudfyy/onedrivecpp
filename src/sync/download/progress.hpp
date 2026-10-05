#pragma once

#include "onedrive/cli/console.hpp"

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

    [[nodiscard]] cli::DownloadProgressMetrics sample(
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

}  // namespace onedrive::sync::detail
