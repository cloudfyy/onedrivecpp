#include "download_space_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

int test_consumed_bytes_release_promises() {
    namespace detail = onedrive::sync::detail;

    detail::DownloadSpaceCoordinator::Lease empty;
    empty.consume(0);

    detail::DownloadSpaceCoordinator coordinator{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto first = coordinator.acquire(70);
    first.consume(30);
    auto second = coordinator.acquire(50);
    try {
        second.consume(51);
        return fail("space lease accepted consumption beyond its reservation");
    } catch (const std::logic_error&) {
    }
    return EXIT_SUCCESS;
}

int test_waits_for_capacity() {
    namespace detail = onedrive::sync::detail;

    std::atomic_uintmax_t available{100};
    detail::DownloadSpaceCoordinator coordinator{
        "/downloads",
        10,
        [&available](const std::filesystem::path&) {
            return available.load(std::memory_order_relaxed);
        }
    };
    auto first = coordinator.acquire(80);
    std::latch started{1};
    std::atomic_bool acquired{false};
    std::jthread waiter{[&] {
        started.count_down();
        auto second = coordinator.acquire(20);
        acquired.store(true, std::memory_order_relaxed);
    }};
    started.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    if (acquired.load(std::memory_order_relaxed)) {
        return fail("space reservation exceeded the available capacity");
    }
    available.store(120, std::memory_order_relaxed);
    first = {};
    waiter.join();
    if (!acquired.load(std::memory_order_relaxed)) {
        return fail("waiting space reservation was not resumed");
    }
    return EXIT_SUCCESS;
}

int test_expands_existing_reservations() {
    namespace detail = onedrive::sync::detail;

    detail::DownloadSpaceCoordinator::Lease inactive;
    try {
        inactive.expand(1);
        return fail("inactive lease accepted an expansion");
    } catch (const std::logic_error&) {
    }

    detail::DownloadSpaceCoordinator coordinator{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto lease = coordinator.acquire(30);
    lease.expand(20);
    if (lease.remaining() != 50) {
        return fail("expanded lease did not track its promised bytes");
    }
    lease.consume(50);
    if (lease.remaining() != 0) {
        return fail("expanded lease did not consume its promised bytes");
    }
    auto second = coordinator.acquire(90);
    if (second.remaining() != 90) {
        return fail("consumed expansion did not release coordinator capacity");
    }

    detail::DownloadSpaceCoordinator zero_initial{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto zero = zero_initial.acquire(0);
    zero.expand(25);
    zero.consume(25);
    if (zero.remaining() != 0) {
        return fail("zero-byte lease could not grow dynamically");
    }

    detail::DownloadSpaceCoordinator insufficient{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto only = insufficient.acquire(80);
    try {
        only.expand(11);
        return fail("single lease expansion exceeded available capacity");
    } catch (const std::runtime_error&) {
    }

    detail::DownloadSpaceCoordinator overflow{
        "/downloads",
        0,
        [](const std::filesystem::path&) {
            return std::numeric_limits<std::uintmax_t>::max();
        }
    };
    auto maximum = overflow.acquire(
        std::numeric_limits<std::uintmax_t>::max()
    );
    try {
        maximum.expand(1);
        return fail("lease expansion overflow was accepted");
    } catch (const std::overflow_error&) {
    }
    return EXIT_SUCCESS;
}

int test_expansion_waits_for_other_leases() {
    namespace detail = onedrive::sync::detail;

    detail::DownloadSpaceCoordinator coordinator{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto first = coordinator.acquire(40);
    auto second = coordinator.acquire(40);
    std::latch started{1};
    std::atomic_bool expanded{false};
    std::jthread waiter{[&] {
        started.count_down();
        first.expand(20);
        expanded.store(true, std::memory_order_relaxed);
    }};
    started.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    if (expanded.load(std::memory_order_relaxed)) {
        return fail("lease expansion exceeded concurrent reservations");
    }
    second = {};
    waiter.join();
    if (!expanded.load(std::memory_order_relaxed) ||
        first.remaining() != 60) {
        return fail("lease expansion did not resume after capacity released");
    }
    return EXIT_SUCCESS;
}

int test_insufficient_capacity_and_cancellation() {
    namespace detail = onedrive::sync::detail;

    detail::DownloadSpaceCoordinator insufficient{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    try {
        static_cast<void>(insufficient.acquire(91));
        return fail("insufficient download capacity was accepted");
    } catch (const std::runtime_error&) {
    }

    detail::DownloadSpaceCoordinator cancelled{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto first = cancelled.acquire(80);
    std::latch started{1};
    std::atomic_bool cancellation_observed{false};
    std::jthread waiter{[&] {
        started.count_down();
        try {
            static_cast<void>(cancelled.acquire(20));
        } catch (const detail::DownloadSpaceCancelledError&) {
            cancellation_observed.store(true, std::memory_order_relaxed);
        }
    }};
    started.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    cancelled.cancel();
    waiter.join();
    if (!cancellation_observed.load(std::memory_order_relaxed)) {
        return fail("waiting space reservation did not observe cancellation");
    }
    try {
        static_cast<void>(cancelled.acquire(1));
        return fail("cancelled coordinator accepted a later reservation");
    } catch (const detail::DownloadSpaceCancelledError&) {
    }
    try {
        static_cast<void>(cancelled.acquire(0));
        return fail("cancelled coordinator accepted a zero-byte lease");
    } catch (const detail::DownloadSpaceCancelledError&) {
    }

    detail::DownloadSpaceCoordinator cancelled_expansion{
        "/downloads",
        10,
        [](const std::filesystem::path&) {
            return std::uintmax_t{100};
        }
    };
    auto expanding = cancelled_expansion.acquire(40);
    auto blocker = cancelled_expansion.acquire(40);
    std::latch expansion_started{1};
    std::atomic_bool expansion_cancelled{false};
    std::jthread expansion_waiter{[&] {
        expansion_started.count_down();
        try {
            expanding.expand(20);
        } catch (const detail::DownloadSpaceCancelledError&) {
            expansion_cancelled.store(true, std::memory_order_relaxed);
        }
    }};
    expansion_started.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    cancelled_expansion.cancel();
    expansion_waiter.join();
    if (!expansion_cancelled.load(std::memory_order_relaxed) ||
        expanding.remaining() != 40 || blocker.remaining() != 40) {
        return fail("waiting lease expansion did not preserve cancellation state");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_consumed_bytes_release_promises();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_waits_for_capacity();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_expands_existing_reservations();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_expansion_waits_for_other_leases();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_insufficient_capacity_and_cancellation();
}
