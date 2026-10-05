#include "sync/download/progress.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

using onedrive::test::fail;

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using Estimator =
        onedrive::sync::detail::DownloadProgressEstimator;

    const Estimator::Clock::time_point started{};
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
    return EXIT_SUCCESS;
}
