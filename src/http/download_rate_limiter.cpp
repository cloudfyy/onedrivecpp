#include "onedrive/http/download_rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace onedrive::http {
namespace {

// Keep short transfers responsive without permitting a large startup spike.
constexpr std::uint64_t maximum_default_burst_bytes =
    std::uint64_t{64} * 1024U;

}  // namespace

TransferRateLimiter::TransferRateLimiter(
    std::uint64_t bytes_per_second,
    std::uint64_t burst_bytes
)
    : bytes_per_second_{bytes_per_second},
      capacity_{
          burst_bytes == 0 ?
              std::min(bytes_per_second, maximum_default_burst_bytes) :
              burst_bytes
      },
      tokens_{static_cast<long double>(capacity_)},
      last_refill_{Clock::now()} {
    if (bytes_per_second_ == 0 || capacity_ == 0 ||
        capacity_ > bytes_per_second_ ||
        capacity_ > maximum_default_burst_bytes) {
        throw std::invalid_argument(
            "transfer rate limiter requires a positive rate and a burst "
            "no larger than one second of traffic or 64 KiB"
        );
    }
}

bool TransferRateLimiter::acquire(
    std::size_t bytes,
    const std::stop_token& stop_token
) {
    if (bytes == 0 || stop_token.stop_requested()) {
        return bytes == 0 && !stop_token.stop_requested();
    }

    std::unique_lock lock{mutex_};

    // A monotonically increasing ticket avoids relying on thread identifiers
    // and lets a cancelled caller be removed from any position in the queue.
    const auto ticket = next_ticket_++;
    waiters_.push_back(ticket);
    const auto is_front = [&] {
        return !waiters_.empty() && waiters_.front() == ticket;
    };
    if (!condition_.wait(lock, stop_token, is_front)) {
        remove_waiter(ticket);
        return false;
    }

    std::uintmax_t remaining = bytes;
    while (remaining != 0) {
        if (stop_token.stop_requested()) {
            remove_waiter(ticket);
            return false;
        }
        const auto now = Clock::now();
        refill(now);
        const auto available = static_cast<std::uint64_t>(
            std::floor(tokens_)
        );
        if (available != 0) {
            const auto granted = std::min<std::uintmax_t>(
                remaining,
                available
            );
            tokens_ -= static_cast<long double>(granted);
            remaining -= granted;
            continue;
        }

        // Large libcurl chunks may exceed the bucket capacity. Wait only for
        // the next useful portion so such chunks can be consumed in stages.
        const auto target = std::min<std::uintmax_t>(
            remaining,
            capacity_
        );
        const auto missing =
            static_cast<long double>(target) - tokens_;
        const auto seconds =
            missing / static_cast<long double>(bytes_per_second_);
        auto delay = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<long double>{seconds}
        );
        if (delay <= Clock::duration::zero()) {
            delay = Clock::duration{1};
        }

        // No predicate can become true merely because time passes. The timed
        // wait exists to wake for token replenishment, while notifications
        // provide prompt cancellation and queue-progress handling.
        condition_.wait_until(
            lock,
            stop_token,
            now + delay,
            [] {
                return false;
            }
        );
        if (stop_token.stop_requested()) {
            remove_waiter(ticket);
            return false;
        }
    }

    // Retain the front position for the complete acquisition. This prevents a
    // busy caller from repeatedly rejoining ahead of an older waiting caller.
    waiters_.pop_front();
    condition_.notify_all();
    return true;
}

void TransferRateLimiter::refill(Clock::time_point now) {
    if (now <= last_refill_) {
        return;
    }
    const auto elapsed =
        std::chrono::duration<long double>(now - last_refill_).count();

    // Fractional tokens preserve sub-byte timing precision between refills;
    // acquire() only grants the integral portion actually available.
    tokens_ = std::min(
        static_cast<long double>(capacity_),
        tokens_ +
            elapsed * static_cast<long double>(bytes_per_second_)
    );
    last_refill_ = now;
}

void TransferRateLimiter::remove_waiter(std::uint64_t ticket) {
    const auto waiter =
        std::ranges::find(waiters_, ticket);

    // Missing tickets indicate an internal queue invariant violation rather
    // than a recoverable caller error.
    if (waiter == waiters_.end()) {
        std::terminate();
    }
    waiters_.erase(waiter);
    condition_.notify_all();
}

}  // namespace onedrive::http
