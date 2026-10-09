#include "support.hpp"

namespace {

using namespace onedrive::test::monitor;

int test_timing() {
    MonitorFixture fixture;
    const auto& root = fixture.root;
    {
        std::ofstream output{root / "existing.txt"};
        output << "existing";
    }
    std::filesystem::create_symlink("existing.txt", root / "existing-link");
    std::atomic_int periodic_runs{0};
    onedrive::monitor::Monitor periodic_monitor{
        root,
        [&](const std::stop_token&) {
            const int run = ++periodic_runs;
            return run <= 2 ? 2 : 0;
        },
        60ms,
        10ms
    };
    std::atomic_int periodic_result{-1};
    std::jthread periodic_worker{[&](std::stop_token stop_token) {
        periodic_result = periodic_monitor.run(stop_token);
    }};
    if (!wait_until([&] { return periodic_runs >= 2; }, 2s, 5ms)) {
        periodic_worker.request_stop();
        return fail("Graph polling interval did not trigger synchronization");
    }
    periodic_worker.request_stop();
    periodic_worker.join();
    if (periodic_result != 0) {
        return fail("periodic monitor did not stop successfully");
    }

    std::atomic_int settled_runs{0};
    std::atomic<std::int64_t> settled_elapsed{-1};
    std::chrono::steady_clock::time_point changed_at;
    onedrive::monitor::Monitor settle_monitor{
        root,
        [&](const std::stop_token&) {
            const int run = ++settled_runs;
            if (run == 2) {
                settled_elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - changed_at
                    )
                        .count();
            }
            return 0;
        },
        80ms,
        180ms
    };
    std::jthread settle_worker{[&](std::stop_token stop_token) {
        static_cast<void>(settle_monitor.run(stop_token));
    }};
    if (!wait_until([&] { return settled_runs == 1; }, 2s, 5ms)) {
        settle_worker.request_stop();
        return fail("settle test monitor did not become ready");
    }
    std::this_thread::sleep_for(20ms);
    changed_at = std::chrono::steady_clock::now();
    {
        std::ofstream output{root / "settle.txt"};
        output << "settled";
    }
    if (!wait_until([&] { return settled_runs >= 2; }, 2s, 5ms)) {
        settle_worker.request_stop();
        return fail("settled local change did not trigger synchronization");
    }
    settle_worker.request_stop();
    settle_worker.join();
    if (settled_elapsed < 140) {
        return fail("Graph polling interrupted the local settle delay");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_timing();
}
