#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>

namespace onedrive::http {

/**
 * A cancellable, thread-safe token bucket shared by concurrent transfers.
 *
 * The bucket is initialized at full capacity, so a newly constructed limiter
 * may allow one initial burst. Tokens are then replenished continuously at
 * bytes_per_second and capped at the configured capacity. Waiting callers are
 * admitted in FIFO order; one caller consumes all tokens needed for its
 * acquisition before the next caller is admitted. This provides fairness
 * between libcurl write callbacks rather than between individual bytes.
 *
 * The class controls aggregate throughput only when all relevant transfers
 * share the same instance.
 */
class TransferRateLimiter final {
public:
    /**
     * Constructs a limiter with the requested sustained rate and burst.
     *
     * A burst_bytes value of zero selects min(bytes_per_second, 64 KiB).
     * An explicit burst must be positive, no larger than one second of traffic,
     * and no larger than 64 KiB. The rate itself must be positive.
     *
     * @throws std::invalid_argument if the rate or burst is invalid.
     */
    explicit TransferRateLimiter(
        std::uint64_t bytes_per_second,
        std::uint64_t burst_bytes = 0
    );

    /**
     * Waits until this caller has consumed tokens for all requested bytes.
     *
     * Requests larger than the bucket capacity are fulfilled incrementally,
     * without requiring the entire request to fit in the bucket at once. A
     * zero-byte request succeeds immediately unless cancellation was already
     * requested.
     *
     * @return true after all tokens are acquired; false if stop_token is
     *         already stopped or becomes stopped while this caller waits.
     */
    [[nodiscard]] bool acquire(
        std::size_t bytes,
        const std::stop_token& stop_token = {}
    );

private:
    using Clock = std::chrono::steady_clock;

    // Both helpers require mutex_ to be held by the caller.
    void refill(Clock::time_point now);
    void remove_waiter(std::uint64_t ticket);

    std::uint64_t bytes_per_second_;
    std::uint64_t capacity_;
    long double tokens_;
    Clock::time_point last_refill_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<std::uint64_t> waiters_;
    std::uint64_t next_ticket_{0};
};

}  // namespace onedrive::http
