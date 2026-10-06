#include "onedrive/http/transfer_rate_limiter.hpp"
#include "support/common.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using onedrive::test::fail;

int test_invalid_and_zero_acquisitions() {
    try {
        const onedrive::http::TransferRateLimiter invalid{0};
        return fail("zero aggregate transfer rate was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        const onedrive::http::TransferRateLimiter invalid_burst{10, 11};
        return fail("oversized aggregate transfer burst was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        const onedrive::http::TransferRateLimiter invalid_capacity{
            100'000,
            65'537,
        };
        return fail("aggregate transfer burst exceeded 64 KiB");
    } catch (const std::invalid_argument&) {
    }

    onedrive::http::TransferRateLimiter limiter{1024, 1};
    if (!limiter.acquire(0)) {
        return fail("zero-byte rate acquisition changed limiter state");
    }
    std::stop_source stopped;
    stopped.request_stop();
    if (limiter.acquire(0, stopped.get_token()) ||
        limiter.acquire(1, stopped.get_token())) {
        return fail("stopped rate acquisition was accepted");
    }
    return EXIT_SUCCESS;
}

int test_concurrent_acquisitions_share_rate() {
    using namespace std::chrono_literals;

    onedrive::http::TransferRateLimiter limiter{2000, 1};
    if (!limiter.acquire(1)) {
        return fail("initial aggregate rate token was unavailable");
    }

    std::latch ready{2};
    std::latch start{1};
    std::atomic_bool first_acquired{false};
    std::atomic_bool second_acquired{false};
    const auto acquire = [&](std::atomic_bool& acquired) {
        ready.count_down();
        start.wait();
        acquired.store(limiter.acquire(100), std::memory_order_relaxed);
    };
    std::jthread first{acquire, std::ref(first_acquired)};
    std::jthread second{acquire, std::ref(second_acquired)};
    ready.wait();
    const auto started_at = std::chrono::steady_clock::now();
    start.count_down();
    first.join();
    second.join();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    if (!first_acquired.load(std::memory_order_relaxed) ||
        !second_acquired.load(std::memory_order_relaxed) ||
        elapsed < 80ms || elapsed > 1s) {
        return fail("concurrent transfers did not share one aggregate rate");
    }
    return EXIT_SUCCESS;
}

int test_waiting_acquisition_is_cancellable() {
    using namespace std::chrono_literals;

    onedrive::http::TransferRateLimiter limiter{1, 1};
    if (!limiter.acquire(1)) {
        return fail("initial cancellation-test token was unavailable");
    }

    std::latch started{1};
    std::atomic_bool acquired{true};
    std::jthread waiter{
        [&](std::stop_token stop_token) {
            started.count_down();
            acquired.store(
                limiter.acquire(1, stop_token),
                std::memory_order_relaxed
            );
        }
    };
    started.wait();
    std::this_thread::sleep_for(20ms);
    const auto cancelled_at = std::chrono::steady_clock::now();
    waiter.request_stop();
    waiter.join();
    if (acquired.load(std::memory_order_relaxed) ||
        std::chrono::steady_clock::now() - cancelled_at > 500ms) {
        return fail("waiting aggregate rate acquisition ignored cancellation");
    }
    return EXIT_SUCCESS;
}

int test_queued_cancellation_does_not_block_following_waiters() {
    using namespace std::chrono_literals;

    onedrive::http::TransferRateLimiter limiter{1000, 1};
    if (!limiter.acquire(1)) {
        return fail("initial queued-cancellation token was unavailable");
    }

    std::latch front_started{1};
    std::atomic_bool front_acquired{false};
    std::jthread front{
        [&] {
            front_started.count_down();
            front_acquired.store(
                limiter.acquire(100),
                std::memory_order_relaxed
            );
        }
    };
    front_started.wait();
    std::this_thread::sleep_for(20ms);

    std::latch queued_started{1};
    std::atomic_bool queued_acquired{true};
    std::jthread queued{
        [&](std::stop_token stop_token) {
            queued_started.count_down();
            queued_acquired.store(
                limiter.acquire(1, stop_token),
                std::memory_order_relaxed
            );
        }
    };
    queued_started.wait();
    std::this_thread::sleep_for(20ms);
    queued.request_stop();
    queued.join();
    front.join();

    if (!front_acquired.load(std::memory_order_relaxed) ||
        queued_acquired.load(std::memory_order_relaxed) ||
        !limiter.acquire(1)) {
        return fail(
            "cancelled queued acquisition blocked following waiters"
        );
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_invalid_and_zero_acquisitions();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_concurrent_acquisitions_share_rate();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_waiting_acquisition_is_cancellable();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_queued_cancellation_does_not_block_following_waiters();
}
