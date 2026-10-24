#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

namespace onedrive::test::gui_monitor {

void test_worker_exclusion_and_application_service() {
    Fixture fixture;
    std::atomic_bool entered{false};
    std::atomic_int monitor_calls{0};
    gui::SyncController controller{
        [&](config::Config, const events::Observer&, std::stop_token token) {
            entered = true;
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(5ms);
            }
            return exit_code::cancelled;
        },
        nullptr,
        [&](config::Config, const events::Observer&, std::stop_token) {
            ++monitor_calls;
            return exit_code::success;
        }
    };
    controller.start(fixture.file, false);
    require(
        wait_until([&] { return entered.load(); }), "single task did not start"
    );
    require(
        test::throws_with<std::logic_error>(
            [&] { controller.start_monitoring(fixture.file); },
            "already running"
        ),
        "monitor ran beside a single synchronization"
    );
    controller.cancel();
    require(
        wait_until([&] { return !controller.running(); }) && monitor_calls == 0,
        "single-task cancellation did not preserve worker exclusion"
    );

    test::app::FakeRuntimeFactory fake;
    fake.monitor_override = [](const config::Config& config,
                               monitor::SyncCallback sync,
                               graph::GraphClient&) {
        return std::make_unique<monitor::FileMonitor>(
            std::in_place_type<monitor::Monitor>,
            config.sync_data_directory,
            std::move(sync),
            30s,
            20ms,
            monitor::NotificationCallbacks{},
            config.state_directory
        );
    };
    fake.delta_override = [](const std::optional<std::string>&,
                             std::stop_token) {
        return graph::DeltaResult{
            .changes = {}, .delta_link = "offline-cursor"
        };
    };
    gui::SyncController monitoring{
        gui::SynchronizationFunction{},
        nullptr,
        [&](config::Config config,
            const events::Observer& observer,
            std::stop_token token) {
            const app::RuntimeFactory factory{util::borrowed_proxy, fake};
            return app::monitor_account(
                std::move(config), factory, observer, false, token
            );
        }
    };
    monitoring.start_monitoring(fixture.file);
    require(
        wait_until([&] {
            const auto reply =
                monitor::find_control(fixture.config.state_directory);
            return reply && reply->state == "waiting";
        }),
        "GUI worker did not use the shared Monitor application service"
    );
    require(
        app::detail::inspect_runtime_owner(fixture.config.state_directory)
            .has_value(),
        "shared service released the state lock while waiting"
    );
    monitoring.cancel();
    require(
        wait_until([&] { return !monitoring.running(); }) &&
            !app::detail::inspect_runtime_owner(fixture.config.state_directory),
        "shared Monitor service did not finish cleanup before worker completion"
    );
}

void test_controller_configuration_generations() {
    Fixture first;
    Fixture second;
    const auto first_lock = app::detail::acquire_runtime_lock(
        first.config.state_directory, "monitor"
    );
    const auto second_lock = app::detail::acquire_runtime_lock(
        second.config.state_directory, "monitor"
    );
    monitor::ControlServer first_server{
        first.config.state_directory, first.config.sync_data_directory
    };
    monitor::ControlServer second_server{
        second.config.state_directory, second.config.sync_data_directory
    };
    gui::MonitorController controller;
    int acknowledgements = 0;
    int failures = 0;
    bool obsolete_status = false;
    QObject::connect(
        &controller,
        &gui::MonitorController::acknowledged,
        &controller,
        [&](auto, const auto&) { ++acknowledgements; }
    );
    QObject::connect(
        &controller,
        &gui::MonitorController::request_failed,
        &controller,
        [&](const auto&) { ++failures; }
    );
    controller.set_state_directory(first.config.state_directory);
    controller.refresh();
    require(
        controller.busy() && !controller.requesting() &&
            test::throws_with<std::logic_error>(
                [&] { controller.request(monitor::ControlCommand::stop, {}); },
                "already running"
            ),
        "a status query did not exclude concurrent control requests"
    );
    controller.set_state_directory(second.config.state_directory);
    QObject::connect(
        &controller, &gui::MonitorController::changed, &controller, [&] {
            obsolete_status |=
                controller.status() && controller.status()->sync_directory ==
                                           first.config.sync_data_directory;
        }
    );
    require(
        wait_until([&] { return !controller.busy() && controller.status(); }) &&
            controller.status()->sync_directory ==
                second.config.sync_data_directory &&
            !obsolete_status && acknowledgements == 0 && failures == 0,
        "an old configuration query overwrote the selected Monitor"
    );

    const auto target = controller.status()->target();
    controller.request(monitor::ControlCommand::trigger, target);
    require(
        controller.requesting(), "an active control request was not observable"
    );
    controller.set_state_directory({});
    require(
        wait_until([&] { return !controller.busy(); }) &&
            !controller.status() && !controller.owner() &&
            controller.error().isEmpty() && !controller.requesting() &&
            acknowledgements == 0 && failures == 0 &&
            second_server.take_trigger(),
        "an obsolete control callback updated a cleared configuration"
    );
    controller.refresh();
    require(!controller.busy(), "cleared configuration started another query");

    controller.set_state_directory(first.config.state_directory);
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy() && controller.status(); }),
        "configuration discovery did not resume after clearing"
    );
    controller.request(
        monitor::ControlCommand::trigger,
        {controller.status()->pid, "obsolete-instance"}
    );
    require(
        wait_until([&] { return !controller.busy(); }) && failures == 1 &&
            acknowledgements == 0 && controller.uncertain() &&
            !controller.runtime_free() &&
            controller.error().contains("owner changed") &&
            controller.status() && !first_server.take_trigger(),
        "a failed control request hid its error or mutated the Monitor"
    );
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) &&
            !controller.uncertain() && controller.error().isEmpty(),
        "a fresh status query did not recover from a rejected request"
    );
}

void test_controller_missing_target_and_stale_endpoint() {
    gui::MonitorController controller;
    controller.refresh();
    require(
        !controller.busy() &&
            test::throws_with<std::logic_error>(
                [&] { controller.request(monitor::ControlCommand::stop, {}); },
                "No state directory"
            ),
        "an unselected Monitor target was queried or accepted a control request"
    );

    Fixture running;
    const auto gates = running.temporary.path() / "gates";
    std::filesystem::create_directory(gates);
    Child child{running, gates};
    child.process.kill();
    require(
        child.process.waitForFinished(5000),
        "cannot stop stale endpoint fixture"
    );
    controller.set_state_directory(running.config.state_directory);
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) &&
            !controller.error().isEmpty() && !controller.uncertain() &&
            controller.runtime_free() && !controller.status() &&
            !controller.owner(),
        "a refused stale endpoint hid its error or retained released ownership"
    );
    const auto lock = app::detail::acquire_runtime_lock(
        running.config.state_directory, "monitor"
    );
    controller.refresh();
    require(
        wait_until([&] { return !controller.busy(); }) && controller.owner() &&
            !controller.runtime_free() && !controller.error().isEmpty(),
        "a refused endpoint bypassed an active startup/cleanup lock"
    );
}

void test_polling_reuses_selected_directory_without_config_logs() {
    Fixture fixture;
    const auto lock = app::detail::acquire_runtime_lock(
        fixture.config.state_directory, "monitor"
    );
    monitor::ControlServer server{
        fixture.config.state_directory, fixture.config.sync_data_directory
    };
    std::atomic_int messages{0};
    std::atomic_int configuration_messages{0};
    const logging::Session logging{{
        .level = "debug",
        .message_sink = [&](logging::Severity, std::string_view message) {
            ++messages;
            if (message == "TOML configuration loaded and validated") {
                ++configuration_messages;
            }
        },
    }};
    const auto config = config::Config::load(fixture.file);
    require(
        configuration_messages == 1,
        "configuration debug logging was disabled instead of fixing polling"
    );
    gui::MonitorController controller;
    controller.set_state_directory(config.state_directory);
    std::filesystem::remove(fixture.file);
    const int before = messages.load();
    for (int iteration = 0; iteration < 3; ++iteration) {
        controller.refresh();
        require(
            wait_until([&] { return !controller.busy(); }) &&
                controller.status() && controller.error().isEmpty() &&
                controller.status()->sync_directory ==
                    config.sync_data_directory,
            "status polling reloaded a removed configuration instead of using "
            "the selected directory"
        );
    }
    controller.request(
        monitor::ControlCommand::trigger, controller.status()->target()
    );
    require(
        wait_until([&] { return !controller.busy(); }) &&
            controller.error().isEmpty() && server.take_trigger(),
        "manual triggering reloaded configuration or lost its selected target"
    );
    controller.request(
        monitor::ControlCommand::stop, controller.status()->target()
    );
    require(
        wait_until([&] { return !controller.busy(); }) &&
            controller.error().isEmpty() &&
            server.stop_token().stop_requested(),
        "manual stopping reloaded configuration or lost its selected target"
    );
    require(
        messages == before && configuration_messages == 1,
        "Monitor status/control polling emitted configuration debug noise"
    );
    spdlog::debug("ordinary debug diagnostic remains enabled");
    require(
        messages == before + 1,
        "fixing Monitor polling suppressed unrelated debug diagnostics"
    );
}

} // namespace onedrive::test::gui_monitor
