#include "sync/core/item_ops.hpp"
#include "support/common.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

using onedrive::test::fail;

bool wait_until(const std::atomic_bool& value) {
    return onedrive::test::wait_until(
        [&] {
            return value.load(std::memory_order_acquire);
        },
        1s,
        0ms
    );
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

    std::atomic_bool same_destination_started{false};
    std::atomic_bool same_destination_acquired{false};
    std::jthread same_destination_worker;
    {
        auto first = coordinator.acquire_destination(
            std::filesystem::current_path() / "Folder/../Target.txt"
        );
        same_destination_worker = std::jthread{[&] {
            same_destination_started.store(true, std::memory_order_release);
            auto second = coordinator.acquire_destination(
                std::filesystem::current_path() / "target.txt"
            );
            same_destination_acquired.store(
                true,
                std::memory_order_release
            );
        }};
        if (!wait_until(same_destination_started)) {
            return fail("same-destination worker did not start");
        }
        std::this_thread::sleep_for(20ms);
        if (same_destination_acquired.load(std::memory_order_acquire)) {
            return fail(
                "equivalent destination paths acquired concurrent leases"
            );
        }
    }
    same_destination_worker.join();
    if (!same_destination_acquired.load(std::memory_order_acquire)) {
        return fail(
            "same-destination waiter did not acquire the released lease"
        );
    }

    std::atomic_bool different_destination_acquired{false};
    {
        auto first = coordinator.acquire_destination("first.txt");
        std::jthread different_destination_worker{[&] {
            auto second = coordinator.acquire_destination("second.txt");
            different_destination_acquired.store(
                true,
                std::memory_order_release
            );
        }};
        if (!wait_until(different_destination_acquired)) {
            return fail("different destinations were unnecessarily serialized");
        }
    }

    try {
        auto lease = coordinator.acquire_destination("exception.txt");
        throw std::runtime_error{"simulated operation failure"};
    } catch (const std::runtime_error&) {
    }
    {
        auto after_exception =
            coordinator.acquire_destination("EXCEPTION.txt");
    }

    std::atomic_bool cancelled_wait_started{false};
    std::atomic_bool cancelled_wait_completed{false};
    {
        auto first = coordinator.acquire("drive", "cancelled-item");
        std::stop_source cancellation;
        std::jthread cancelled_waiter{[&] {
            cancelled_wait_started.store(true, std::memory_order_release);
            try {
                static_cast<void>(coordinator.acquire(
                    "drive",
                    "cancelled-item",
                    cancellation.get_token()
                ));
            } catch (const onedrive::sync::detail::
                         ItemOperationCancelledError&) {
                cancelled_wait_completed.store(
                    true,
                    std::memory_order_release
                );
            }
        }};
        if (!wait_until(cancelled_wait_started)) {
            return fail("cancelled operation waiter did not start");
        }
        cancellation.request_stop();
        if (!wait_until(cancelled_wait_completed)) {
            return fail("operation waiter did not observe cancellation");
        }
    }

    cancelled_wait_started.store(false, std::memory_order_release);
    cancelled_wait_completed.store(false, std::memory_order_release);
    {
        auto first =
            coordinator.acquire_destination("cancelled-destination.txt");
        std::stop_source cancellation;
        std::jthread cancelled_waiter{[&] {
            cancelled_wait_started.store(true, std::memory_order_release);
            try {
                static_cast<void>(coordinator.acquire_destination(
                    "cancelled-destination.txt",
                    cancellation.get_token()
                ));
            } catch (const onedrive::sync::detail::
                         ItemOperationCancelledError&) {
                cancelled_wait_completed.store(
                    true,
                    std::memory_order_release
                );
            }
        }};
        if (!wait_until(cancelled_wait_started)) {
            return fail("cancelled destination waiter did not start");
        }
        cancellation.request_stop();
        if (!wait_until(cancelled_wait_completed)) {
            return fail("destination waiter did not observe cancellation");
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
    try {
        static_cast<void>(
            coordinator.acquire_destination(std::filesystem::path{})
        );
        return fail("empty destination path was accepted");
    } catch (const std::invalid_argument&) {
    }

    return EXIT_SUCCESS;
}
