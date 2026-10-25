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
    return static_cast<std::uint64_t>(std::round(value));
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

TransferProgressEstimator::TransferProgressEstimator(
    Clock::time_point started_at
)
    : started_at_{started_at},
      last_sample_at_{started_at} {
}

events::DownloadProgressMetrics TransferProgressEstimator::sample(
    std::uint64_t downloaded, std::uint64_t total, Clock::time_point sampled_at
) {
    return sample(downloaded, total, downloaded, sampled_at);
}

events::TransferProgressMetrics TransferProgressEstimator::sample(
    std::uint64_t completed,
    std::uint64_t total,
    std::uint64_t transferred,
    Clock::time_point sampled_at
) {
    if (sampled_at < last_sample_at_) {
        throw std::invalid_argument(
            "transfer progress sample time moved backwards"
        );
    }
    if (transferred < last_downloaded_) {
        throw std::invalid_argument(
            "transfer progress byte count moved backwards"
        );
    }

    const auto sample_duration = sampled_at - last_sample_at_;
    const auto sample_seconds =
        std::chrono::duration<double>(sample_duration).count();
    const auto additional_bytes = transferred - last_downloaded_;
    if (sample_seconds > 0.0 &&
        (additional_bytes != 0 || smoothed_bytes_per_second_)) {
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
    last_downloaded_ = transferred;

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
    if (completed < total && smoothed_bytes_per_second_.has_value() &&
        *smoothed_bytes_per_second_ > 0.0) {
        metrics.estimated_seconds_remaining = bounded_ceil(
            static_cast<double>(total - completed) / *smoothed_bytes_per_second_
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

namespace {
std::uint64_t
add_transfer_bytes(std::uint64_t value, std::uint64_t additional) {
    if (additional > std::numeric_limits<std::uint64_t>::max() - value) {
        throw std::overflow_error{"upload progress byte count overflow"};
    }
    return value + additional;
}
} // namespace

UploadProgressTracker::UploadProgressTracker(
    const std::vector<std::uint64_t>& sizes, const events::Observer& observer
)
    : observer_{observer},
      file_count_{sizes.size()} {
    files_.reserve(sizes.size());
    for (const auto size : sizes) {
        files_.push_back(File{size});
        total_ = add_transfer_bytes(total_, size);
    }
    if (file_count_ != 0) {
        observer_.sync_phase(events::SyncPhase::uploading);
        report(true);
    }
}

void UploadProgressTracker::report(bool force) {
    const auto now = TransferProgressEstimator::Clock::now();
    const auto state = finished_ == file_count_ ? util::ProgressState::completed
                                                : util::ProgressState::ongoing;
    if (!force &&
        !reporter_.should_report(
            finished_, file_count_, completed_, total_, state, now
        ) &&
        !(transferred_ != 0 &&
          now - last_reported_at_ >= std::chrono::seconds{1})) {
        return;
    }
    last_reported_at_ = now;
    observer_.upload_progress({
        .completed_files = finished_,
        .file_count = file_count_,
        .uploaded = completed_,
        .total = total_,
        .state = state,
        .metrics = estimator_.sample(completed_, total_, transferred_, now),
    });
}

void UploadProgressTracker::update(
    std::size_t index,
    std::uint64_t completed,
    std::uint64_t total,
    std::uint64_t transferred
) {
    const std::scoped_lock lock{mutex_};
    auto& file = files_.at(index);
    if (transferred < file.transferred || file.finished) {
        throw std::logic_error{"invalid upload progress transition"};
    }
    total_ = add_transfer_bytes(total_ - file.size, total);
    file.size = total;
    const auto current = std::min(total, std::max(completed, file.completed));
    completed_ = add_transfer_bytes(completed_ - file.completed, current);
    file.completed = current;
    transferred_ =
        add_transfer_bytes(transferred_, transferred - file.transferred);
    file.transferred = transferred;
    report();
}

void UploadProgressTracker::finish(std::size_t index) {
    const std::scoped_lock lock{mutex_};
    auto& file = files_.at(index);
    if (!file.finished) {
        completed_ = add_transfer_bytes(completed_, file.size - file.completed);
        file.completed = file.size;
        file.finished = true;
        ++finished_;
        report();
    }
}

void UploadProgressTracker::skip(std::size_t index) {
    const std::scoped_lock lock{mutex_};
    auto& file = files_.at(index);
    if (!file.finished) {
        total_ -= file.size;
        completed_ -= file.completed;
        file.finished = true;
        --file_count_;
        report(true);
    }
}

}  // namespace onedrive::sync::detail
