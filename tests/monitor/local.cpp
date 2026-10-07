#include "support.hpp"

namespace {

using namespace onedrive::test::monitor;

int test_local() {
    MonitorFixture fixture;
    const auto& temporary = fixture.temporary;
    const auto& root = fixture.root;
    std::atomic_int local_runs{0};
    onedrive::monitor::Monitor local_monitor{
        root,
        [&] {
            ++local_runs;
            return 0;
        },
        5s,
        60ms
    };
    std::atomic_int local_result{-1};
    std::jthread local_worker{[&](std::stop_token stop_token) {
        local_result = local_monitor.run(stop_token);
    }};
    if (!wait_until([&] { return local_runs == 1; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("monitor did not run its initial synchronization");
    }
    std::this_thread::sleep_for(20ms);

    const auto local_file = root / "local.txt";
    {
        std::ofstream output{local_file};
        output << "one";
    }
    {
        std::ofstream output{local_file, std::ios::app};
        output << "two";
    }
    {
        std::ofstream output{local_file, std::ios::app};
        output << "three";
    }
    if (!wait_until([&] { return local_runs >= 2; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("local filesystem changes did not trigger synchronization");
    }
    std::this_thread::sleep_for(150ms);
    if (local_runs != 2) {
        local_worker.request_stop();
        return fail("local filesystem burst was not coalesced");
    }

    const auto nested = root / "new" / "nested";
    std::filesystem::create_directories(nested);
    {
        std::ofstream output{nested / "file.txt"};
        output << "nested";
    }
    if (!wait_until([&] { return local_runs >= 3; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("new directory subtree did not trigger synchronization");
    }
    std::this_thread::sleep_for(100ms);
    const int runs_before_rename = local_runs;
    std::filesystem::rename(nested / "file.txt", nested / "renamed.txt");
    std::filesystem::remove(nested / "renamed.txt");
    if (!wait_until([&] { return local_runs > runs_before_rename; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("local rename and removal did not trigger synchronization");
    }
    std::this_thread::sleep_for(100ms);
    const int runs_before_directory_removal = local_runs;
    std::filesystem::remove_all(root / "new");
    if (!wait_until(
            [&] { return local_runs > runs_before_directory_removal; }, 2s, 5ms
        )) {
        local_worker.request_stop();
        return fail(
            "watched directory removal did not trigger synchronization"
        );
    }
    std::this_thread::sleep_for(100ms);
    const int runs_before_symlink = local_runs;
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto linked = root / "linked";
    std::filesystem::create_directory_symlink(outside, linked);
    {
        std::ofstream output{outside / "ignored.txt"};
        output << "outside";
    }
    std::this_thread::sleep_for(150ms);
    if (local_runs != runs_before_symlink) {
        local_worker.request_stop();
        return fail("monitor followed a directory symlink");
    }
    local_worker.request_stop();
    local_worker.join();
    if (local_result != 0) {
        return fail("stop token did not end local monitoring successfully");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_local();
}
