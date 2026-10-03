#include "download_space_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <latch>
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
    return test_insufficient_capacity_and_cancellation();
}
