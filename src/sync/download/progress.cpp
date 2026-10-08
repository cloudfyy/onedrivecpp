#include "sync/download/progress.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace onedrive::sync::detail {
namespace {

constexpr double smoothing_weight = 0.25;
constexpr auto progress_refresh_interval = std::chrono::seconds{1};

std::uint64_t bounded_round(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    const auto maximum =
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (value >= maximum) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::llround(value));
}

std::uint64_t bounded_ceil(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    const auto maximum =
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (value >= maximum) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::ceil(value));
}

}  // namespace

DownloadProgressEstimator::DownloadProgressEstimator(
    Clock::time_point started_at
)
    : started_at_{started_at},
      last_sample_at_{started_at} {}

events::DownloadProgressMetrics DownloadProgressEstimator::sample(
    std::uint64_t downloaded, std::uint64_t total, Clock::time_point sampled_at
) {
    if (sampled_at < last_sample_at_) {
        throw std::invalid_argument(
            "download progress sample time moved backwards"
        );
    }
    if (downloaded < last_downloaded_) {
        throw std::invalid_argument(
            "download progress byte count moved backwards"
        );
    }

    const auto sample_duration = sampled_at - last_sample_at_;
    const auto sample_seconds =
        std::chrono::duration<double>(sample_duration).count();
    const auto additional_bytes = downloaded - last_downloaded_;
    if (sample_seconds > 0.0 && additional_bytes != 0) {
        const auto instantaneous =
            static_cast<double>(additional_bytes) / sample_seconds;
        smoothed_bytes_per_second_ =
            smoothed_bytes_per_second_.has_value() ?
                smoothing_weight * instantaneous +
                    (1.0 - smoothing_weight) *
                        *smoothed_bytes_per_second_ :
                instantaneous;
    }
    last_sample_at_ = sampled_at;
    last_downloaded_ = downloaded;

    events::DownloadProgressMetrics metrics{
        .bytes_per_second = smoothed_bytes_per_second_.has_value()
                                ? bounded_round(*smoothed_bytes_per_second_)
                                : 0,
        .estimated_seconds_remaining = std::nullopt,
        .elapsed_milliseconds =
            static_cast<std::uint64_t>(std::max<std::int64_t>(
                0,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    sampled_at - started_at_
                )
                    .count()
            )),
    };
    if (downloaded < total &&
        smoothed_bytes_per_second_.has_value() &&
        *smoothed_bytes_per_second_ > 0.0) {
        metrics.estimated_seconds_remaining = bounded_ceil(
            static_cast<double>(total - downloaded) /
            *smoothed_bytes_per_second_
        );
    }
    return metrics;
}

DownloadProgressReporter::DownloadProgressReporter(
    Clock::time_point started_at
)
    : last_reported_at_{started_at} {}

bool DownloadProgressReporter::should_report(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state,
    Clock::time_point sampled_at
) {
    if (sampled_at < last_reported_at_) {
        throw std::invalid_argument(
            "download progress report time moved backwards"
        );
    }
    const bool changed =
        completed_files != last_observed_completed_files_ ||
        downloaded != last_observed_downloaded_;
    last_observed_completed_files_ = completed_files;
    last_observed_downloaded_ = downloaded;

    const auto percentage = events::download_progress_percentage(
        completed_files, file_count, downloaded, total, state
    );
    const bool completed = state == util::ProgressState::completed;
    const bool due =
        completed ||
        (!reported_ && changed) ||
        (changed &&
         (percentage != last_reported_percentage_ ||
          sampled_at - last_reported_at_ >= progress_refresh_interval));
    if (!due) {
        return false;
    }
    last_reported_at_ = sampled_at;
    last_reported_percentage_ = percentage;
    reported_ = true;
    return true;
}

}  // namespace onedrive::sync::detail
