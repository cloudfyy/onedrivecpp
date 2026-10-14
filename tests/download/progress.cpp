#include "sync/download/progress.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

using onedrive::test::fail;

class UploadObserver final : public onedrive::events::Observer {
public:
    mutable std::vector<onedrive::events::UploadProgressEvent> progress;

private:
    void on_event(const onedrive::events::Event& event) const override {
        if (const auto* value =
                std::get_if<onedrive::events::UploadProgressEvent>(&event)) {
            progress.push_back(*value);
        }
    }
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using Estimator =
        onedrive::sync::detail::DownloadProgressEstimator;
    using Reporter =
        onedrive::sync::detail::DownloadProgressReporter;

    const Estimator::Clock::time_point started{};
    Estimator large{started};
    const auto large_bytes = (std::uint64_t{1} << 63U) + 16'384;
    if (large.sample(large_bytes, large_bytes, started + 1s).bytes_per_second !=
        large_bytes) {
        return fail("large transfer rates overflowed signed rounding");
    }
    Estimator resumed{started};
    const auto history = resumed.sample(900, 1000, 0, started + 1s);
    const auto current = resumed.sample(950, 1000, 50, started + 2s);
    const auto idle = resumed.sample(950, 1000, 50, started + 3s);
    if (history.bytes_per_second != 0 || history.estimated_seconds_remaining ||
        current.bytes_per_second != 50 ||
        current.estimated_seconds_remaining != 1 ||
        idle.bytes_per_second >= current.bytes_per_second) {
        return fail(
            "transfer estimator counted history or kept a stale idle speed"
        );
    }
    UploadObserver observer;
    onedrive::sync::detail::UploadProgressTracker uploads{{1000, 0}, observer};
    uploads.update(0, 900, 1000, 0);
    if (observer.progress.back().metrics.bytes_per_second != 0 ||
        observer.progress.back().metrics.estimated_seconds_remaining) {
        return fail(
            "resumed upload baseline generated fictitious speed or ETA"
        );
    }
    uploads.finish(0);
    if (observer.progress.back().state !=
            onedrive::util::ProgressState::ongoing ||
        observer.progress.back().completed_files != 1) {
        return fail("upload batch completed before its zero-byte file");
    }
    uploads.finish(1);
    const auto count = observer.progress.size();
    uploads.finish(1);
    if (observer.progress.back().state !=
            onedrive::util::ProgressState::completed ||
        observer.progress.back().completed_files != 2 ||
        observer.progress.back().uploaded != 1000 ||
        observer.progress.size() != count) {
        return fail("upload batch lost completion or counted a file twice");
    }
    UploadObserver skipped_observer;
    onedrive::sync::detail::UploadProgressTracker skipped{
        {3, 4}, skipped_observer
    };
    skipped.update(1, 4, 4, 4);
    skipped.finish(1);
    skipped.skip(0);
    if (skipped_observer.progress.back().file_count != 1 ||
        skipped_observer.progress.back().completed_files != 1 ||
        skipped_observer.progress.back().total != 4 ||
        skipped_observer.progress.back().state !=
            onedrive::util::ProgressState::completed) {
        return fail(
            "disappeared upload source left a fictitious pending transfer"
        );
    }
    Estimator estimator{started};
    const auto first = estimator.sample(100, 1'000, started + 1s);
    const auto second = estimator.sample(300, 1'000, started + 2s);
    const auto completed = estimator.sample(1'000, 1'000, started + 4s);
    if (first.bytes_per_second != 100 ||
        first.estimated_seconds_remaining != 9 ||
        first.elapsed_milliseconds != 1'000 ||
        second.bytes_per_second != 125 ||
        second.estimated_seconds_remaining != 6 ||
        second.elapsed_milliseconds != 2'000 ||
        completed.bytes_per_second != 181 ||
        completed.estimated_seconds_remaining.has_value() ||
        completed.elapsed_milliseconds != 4'000) {
        return fail("download progress smoothing or ETA was incorrect");
    }

    const auto unchanged = estimator.sample(
        1'000,
        1'000,
        started + 4s
    );
    if (unchanged.bytes_per_second != completed.bytes_per_second ||
        unchanged.estimated_seconds_remaining.has_value()) {
        return fail("unchanged download progress altered the estimate");
    }

    try {
        static_cast<void>(
            estimator.sample(999, 1'000, started + 5s)
        );
        return fail("backwards download bytes were accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(
            estimator.sample(1'000, 1'000, started + 3s)
        );
        return fail("backwards progress time was accepted");
    } catch (const std::invalid_argument&) {
    }

    Reporter reporter{started};
    if (!reporter.should_report(
            801,
            13'342,
            459'700'000,
            17'395'200'000,
            onedrive::util::ProgressState::ongoing,
            started + 100ms
        ) ||
        reporter.should_report(
            934,
            13'342,
            473'600'000,
            17'395'200'000,
            onedrive::util::ProgressState::ongoing,
            started + 200ms
        ) ||
        !reporter.should_report(
            1'068,
            13'342,
            487'100'000,
            17'395'200'000,
            onedrive::util::ProgressState::ongoing,
            started + 1100ms
        )) {
        return fail(
            "file-count progress bypassed the timed refresh interval"
        );
    }
    if (!reporter.should_report(
            1'068,
            13'342,
            521'856'000,
            17'395'200'000,
            onedrive::util::ProgressState::ongoing,
            started + 1200ms
        )) {
        return fail("displayed percentage change was not reported");
    }
    if (reporter.should_report(
            1'068,
            13'342,
            521'856'000,
            17'395'200'000,
            onedrive::util::ProgressState::ongoing,
            started + 2200ms
        )) {
        return fail("unchanged progress was reported");
    }
    if (!reporter.should_report(
            13'342,
            13'342,
            17'395'200'000,
            17'395'200'000,
            onedrive::util::ProgressState::completed,
            started + 2300ms
        )) {
        return fail("completed progress was not reported");
    }
    return EXIT_SUCCESS;
}
