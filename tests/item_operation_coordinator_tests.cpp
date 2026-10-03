#include "item_operation_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool wait_until(const std::atomic_bool& value) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}

}  // namespace

int main() {
    using onedrive::sync::detail::ItemOperationCoordinator;

    ItemOperationCoordinator coordinator;
    std::atomic_bool same_key_started{false};
    std::atomic_bool same_key_acquired{false};
    std::jthread same_key_worker;
    {
        auto first = coordinator.acquire("drive", "item");
        same_key_worker = std::jthread{[&] {
            same_key_started.store(true, std::memory_order_release);
            auto second = coordinator.acquire("drive", "item");
            same_key_acquired.store(true, std::memory_order_release);
        }};
        if (!wait_until(same_key_started)) {
            return fail("same-key worker did not start");
        }
        std::this_thread::sleep_for(20ms);
        if (same_key_acquired.load(std::memory_order_acquire)) {
            return fail("same item acquired two concurrent operation leases");
        }
    }
    same_key_worker.join();
    if (!same_key_acquired.load(std::memory_order_acquire)) {
        return fail("same-key waiter did not acquire the released lease");
    }

    std::atomic_bool different_key_acquired{false};
    {
        auto first = coordinator.acquire("drive", "item");
        std::jthread different_key_worker{[&] {
            auto second = coordinator.acquire("drive", "other-item");
            different_key_acquired.store(true, std::memory_order_release);
        }};
        if (!wait_until(different_key_acquired)) {
            return fail("different items were unnecessarily serialized");
        }
    }

    std::atomic_bool different_drive_acquired{false};
    {
        auto first = coordinator.acquire("drive", "item");
        std::jthread different_drive_worker{[&] {
            auto second = coordinator.acquire("other-drive", "item");
            different_drive_acquired.store(true, std::memory_order_release);
        }};
        if (!wait_until(different_drive_acquired)) {
            return fail("same item ID on different drives was serialized");
        }
    }

    try {
        static_cast<void>(coordinator.acquire("", "item"));
        return fail("empty drive ID was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(coordinator.acquire("drive", ""));
        return fail("empty remote ID was accepted");
    } catch (const std::invalid_argument&) {
    }

    return EXIT_SUCCESS;
}
