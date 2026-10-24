#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

namespace onedrive::test::gui_monitor {

void test_readonly_discovery_and_unsafe_endpoint() {
    Fixture fixture;
    const auto path = fixture.config.state_directory / "onedrive-cpp.lock";
    const auto read = [&] {
        std::ifstream input{path, std::ios::binary};
        return std::string(std::istreambuf_iterator<char>{input}, {});
    };
    {
        const auto lock = app::detail::acquire_runtime_lock(
            fixture.config.state_directory, "monitor"
        );
        const auto metadata = read();
        const auto owner =
            app::detail::inspect_runtime_owner(fixture.config.state_directory);
        require(
            owner && owner->operation == "monitor" && read() == metadata,
            "read-only ownership inspection changed active lock metadata"
        );
    }
    const auto metadata = read();
    struct stat before{}, after{};
    require(
        ::lstat(path.c_str(), &before) == 0,
        "cannot inspect runtime lock identity"
    );
    require(
        !app::detail::inspect_runtime_owner(fixture.config.state_directory) &&
            read() == metadata && ::lstat(path.c_str(), &after) == 0 &&
            before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
            before.st_mode == after.st_mode,
        "read-only ownership inspection rewrote a free lock inode"
    );
    test::write_file(
        fixture.config.state_directory / "monitor-control.sock", "not a socket"
    );
    gui::MonitorController controller;
    controller.set_state_directory(fixture.config.state_directory);
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) &&
            !controller.status() && controller.uncertain() &&
            !controller.error().isEmpty(),
        "unsafe IPC endpoint was presented as an idle successful monitor"
    );
    {
        int engines = 0;
        const auto run =
            [&](config::Config, const events::Observer&, std::stop_token) {
                ++engines;
                return exit_code::success;
            };
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file), {}, nullptr, run, run
        };
        require(
            wait_until([&] {
                return text(window, "monitorStatus")
                    .contains("Monitor status unavailable");
            }),
            "unsafe endpoint was not exposed in the GUI"
        );
        require(
            !text(window, "monitorStatus").contains("Monitor: not running."),
            "an invalid IPC endpoint was presented as idle in the GUI"
        );
        require(
            !button(window, "startMonitorButton")->isEnabled() &&
                !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "previewSyncButton")->isEnabled(),
            "uncertain ownership allowed a new GUI task"
        );
        button(window, "startMonitorButton")->click();
        button(window, "startSyncButton")->click();
        require(
            engines == 0, "uncertain ownership started a synchronization engine"
        );
        require(
            button(window, "saveSettingsButton")->isEnabled(),
            "known-free runtime resources blocked settings recovery"
        );
        const auto lock = app::detail::acquire_runtime_lock(
            fixture.config.state_directory, "monitor"
        );
        controller.refresh();
        require(
            wait_until([&] { return !controller.busy(); }) &&
                controller.owner() && !controller.runtime_free(),
            "an IPC error hid an occupied runtime lock"
        );
        require(
            wait_until([&] {
                return !button(window, "saveSettingsButton")->isEnabled();
            }),
            "an IPC error allowed settings changes during an active task"
        );
    }
    std::filesystem::remove(
        fixture.config.state_directory / "monitor-control.sock"
    );
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) &&
            controller.error().isEmpty() && !controller.uncertain() &&
            !controller.status(),
        "Monitor discovery did not recover after an explicit endpoint repair"
    );
}

void test_readonly_owner_errors() {
    Fixture fixture;
    const auto path = fixture.config.state_directory / "onedrive-cpp.lock";
    std::filesystem::create_symlink(fixture.file, path);
    require(
        test::throws_with<std::system_error>(
            [&] {
                static_cast<void>(app::detail::inspect_runtime_owner(
                    fixture.config.state_directory
                ));
            },
            "cannot safely open path"
        ),
        "ownership inspection followed a lock symlink"
    );
    std::filesystem::remove(path);
    const auto lock = app::detail::acquire_runtime_lock(
        fixture.config.state_directory, "monitor"
    );
    auto metadata = nlohmann::json::parse(test::read_file(path));
    metadata["pid"] = 0;
    test::write_file(path, metadata.dump());
    require(
        test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(app::detail::inspect_runtime_owner(
                    fixture.config.state_directory
                ));
            },
            "invalid lock owner metadata"
        ),
        "ownership inspection accepted an invalid occupied owner identity"
    );
    test::write_file(path, "invalid metadata");
    gui::MonitorController controller;
    controller.set_state_directory(fixture.config.state_directory);
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) &&
            controller.uncertain() && !controller.runtime_free() &&
            !controller.error().isEmpty(),
        "corrupt occupied lock metadata was treated as released resources"
    );
    std::filesystem::create_hard_link(path, path.string() + ".alias");
    require(
        test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(app::detail::inspect_runtime_owner(
                    fixture.config.state_directory
                ));
            },
            "single-link"
        ),
        "ownership inspection accepted a hard-linked lock"
    );
}

} // namespace onedrive::test::gui_monitor
