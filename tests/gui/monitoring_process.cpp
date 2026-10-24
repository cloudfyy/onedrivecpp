#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

namespace onedrive::test::gui_monitor {

int child(
    const std::filesystem::path& file, const std::filesystem::path& gates
) {
    auto config = config::Config::load(file);
    const auto identity = test::test_drive_identity("me");
    const app::detail::RuntimePreflight preflight{
        config, app::detail::Operation::monitor
    };
    const auto root = account::AccountState::drive_data_directory(
        config.sync_data_directory, identity
    );
    const Observer observer;
    const auto lease = app::detail::acquire_sync_directory_lock(
        root, config.state_directory, "monitor", observer
    );
    std::filesystem::create_directories(root);
    int rounds = 0;
    monitor::Monitor instance{
        root,
        [&](const std::stop_token& token) {
            ++rounds;
            test::write_file(gates / "round", std::to_string(rounds));
            while (!std::filesystem::exists(
                gates / ("release-" + std::to_string(rounds))
            )) {
                if (token.stop_requested()) {
                    return exit_code::cancelled;
                }
                std::this_thread::sleep_for(5ms);
            }
            return rounds == 1 ? exit_code::blocked : exit_code::success;
        },
        30s,
        20ms,
        {},
        config.state_directory
    };
    const int result = instance.run(std::stop_token{});
    test::write_file(gates / "cleanup", "");
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!std::filesystem::exists(gates / "release-cleanup")) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "external Monitor cleanup fixture timed out"
            );
        }
        std::this_thread::sleep_for(5ms);
    }
    return result;
}

} // namespace onedrive::test::gui_monitor
