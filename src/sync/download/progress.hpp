#pragma once

#include "onedrive/ui/common/observer.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <mutex>
#include <vector>

namespace onedrive::sync::detail {

class TransferProgressEstimator final {
public:
    using Clock = std::chrono::steady_clock;

    explicit TransferProgressEstimator(
        Clock::time_point started_at = Clock::now()
    );

    [[nodiscard]] events::DownloadProgressMetrics sample(
        std::uint64_t downloaded,
        std::uint64_t total,
        Clock::time_point sampled_at = Clock::now()
    );
    [[nodiscard]] events::TransferProgressMetrics sample(
        std::uint64_t completed,
        std::uint64_t total,
        std::uint64_t transferred,
        Clock::time_point sampled_at
    );

private:
    Clock::time_point started_at_;
    Clock::time_point last_sample_at_;
    std::uint64_t last_downloaded_{0};
    std::optional<double> smoothed_bytes_per_second_;
};

using DownloadProgressEstimator = TransferProgressEstimator;

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

using TransferProgressReporter = DownloadProgressReporter;

class UploadProgressTracker final {
public:
    UploadProgressTracker(
        const std::vector<std::uint64_t>& sizes,
        const events::Observer& observer
    );
    void update(
        std::size_t index,
        std::uint64_t completed,
        std::uint64_t total,
        std::uint64_t transferred
    );
    void finish(std::size_t index);
    void skip(std::size_t index);

private:
    struct File {
        std::uint64_t size{0};
        std::uint64_t completed{0};
        std::uint64_t transferred{0};
        bool finished{false};
    };
    void report(bool force = false);
    const events::Observer& observer_;
    std::mutex mutex_;
    std::vector<File> files_;
    std::uint64_t total_{0};
    std::uint64_t completed_{0};
    std::uint64_t transferred_{0};
    std::size_t file_count_{0};
    std::size_t finished_{0};
    TransferProgressEstimator estimator_;
    TransferProgressReporter reporter_;
    TransferProgressEstimator::Clock::time_point last_reported_at_{};
};

}  // namespace onedrive::sync::detail
