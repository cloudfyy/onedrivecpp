#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

namespace onedrive::test::gui_monitor {

void test_external_window() {
    Fixture fixture;
    const auto gates = fixture.temporary.path() / "gates";
    std::filesystem::create_directory(gates);
    Child child{fixture, gates};
    gui::Localization localization{gui::Language::english};
    int engines = 0;
    const auto sync =
        [&](config::Config, const events::Observer&, std::stop_token) {
            ++engines;
            return exit_code::success;
        };
    {
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            &localization,
            sync,
            sync
        };
        window.show();
        require(
            wait_until([&] {
                return text(window, "monitorStatus")
                    .contains("External Monitor");
            }),
            "GUI did not discover the same-state external Monitor"
        );
        require(
            !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "startMonitorButton")->isEnabled(),
            "external active round permitted another engine"
        );
        window.close();
        require(
            !window.isVisible() && child.process.state() == QProcess::Running &&
                monitor::request_control(
                    fixture.config.state_directory,
                    monitor::ControlCommand::status
                )
                        .state == "synchronizing",
            "closing GUI stopped an external task"
        );
    }
    std::filesystem::remove(fixture.token);
    test::write_file(gates / "release-1", "");
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        sync,
        sync
    };
    window.show();
    require(
        wait_until([&] {
            return button(window, "startSyncButton")->isEnabled();
        }),
        "external controls incorrectly required saved authentication"
    );
    require(
        text(window, "syncStatus").contains("Last round incomplete") &&
            text(window, "monitorTotals").isEmpty() &&
            text(window, "syncSummary").isEmpty() &&
            text(window, "syncContext")
                .contains("not available from external") &&
            text(window, "monitorStatus")
                .contains("not live transfer progress"),
        "external snapshot fabricated direction/progress or hid blocked results"
    );
    button(window, "startSyncButton")->click();
    require(
        wait_until([&] { return read_round(gates) == 2; }) && engines == 0,
        "external GUI trigger started a parallel engine"
    );
    QProcess cli;
    cli.start(
        ONEDRIVE_CLI_EXECUTABLE,
        {"monitor",
         "status",
         "--config",
         QString::fromStdString(fixture.file.string()),
         "--output",
         "json"}
    );
    require(
        cli.waitForFinished(5000) && cli.exitCode() == 0 &&
            cli.readAllStandardOutput().contains(
                "\"event\":\"monitor_control\""
            ),
        "GUI and CLI did not share the same external Monitor"
    );
    require(
        wait_until([&] {
            return button(window, "stopSyncButton")->isEnabled();
        }),
        "external stop remained disabled after IPC completion"
    );
    bool confirmed = false;
    QTimer::singleShot(0, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            confirmed = true;
            dialog->button(QMessageBox::No)->click();
        }
    });
    button(window, "stopSyncButton")->click();
    require(
        confirmed && child.process.state() == QProcess::Running &&
            monitor::request_control(
                fixture.config.state_directory, monitor::ControlCommand::status
            )
                    .state == "synchronizing",
        "declining external-stop confirmation changed the external task"
    );
    confirmed = false;
    QTimer::singleShot(0, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            confirmed = true;
            dialog->button(QMessageBox::Yes)->click();
        }
    });
    button(window, "stopSyncButton")->click();
    require(
        confirmed && wait_until([&] {
            return std::filesystem::exists(gates / "cleanup");
        }),
        "explicit external stop was not acknowledged"
    );
    require(
        wait_until([&] {
            return text(window, "monitorStatus").contains("startup or cleanup");
        }) &&
            text(window, "syncStatus").contains("Stopping") &&
            !button(window, "saveSettingsButton")->isEnabled() &&
            app::detail::inspect_runtime_owner(fixture.config.state_directory)
                .has_value(),
        "missing endpoint was mistaken for completed external cleanup"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .contains("不表示");
        }),
        "request acknowledgements lost their completion distinction after "
        "retranslation"
    );
    window.close();
    require(
        !window.isVisible() && child.process.state() == QProcess::Running,
        "GUI close interfered with external cleanup"
    );
    test::write_file(gates / "release-cleanup", "");
    require(
        child.process.waitForFinished(5000) && child.process.exitCode() == 0 &&
            !app::detail::inspect_runtime_owner(
                fixture.config.state_directory
            ) &&
            engines == 0,
        "external shutdown did not release resources"
    );
}

void test_overlapping_root_is_not_attached() {
    Fixture owner;
    Fixture contender;
    test::write_file(
        contender.file,
        "config_version = 2\n[auth]\napplication_id = \"test-client\"\n"
        "[sync]\nmode = \"download_only\"\ndata_directory = \"" +
            owner.config.sync_data_directory.string() +
            "\"\n[state]\ndirectory = \"" +
            contender.config.state_directory.string() + "\"\n"
    );
    const auto gates = owner.temporary.path() / "gates";
    std::filesystem::create_directory(gates);
    Child child{owner, gates};
    test::app::FakeRuntimeFactory fake;
    gui::MainWindow window{
        gui::load_app_state_view_model(contender.file),
        {},
        nullptr,
        {},
        [&](config::Config config,
            const events::Observer& observer,
            std::stop_token token) {
            const app::RuntimeFactory factory{util::borrowed_proxy, fake};
            return app::monitor_account(
                std::move(config), factory, observer, false, token
            );
        }
    };
    window.show();
    button(window, "startMonitorButton")->click();
    require(
        wait_until([&] {
            return text(window, "syncStatus").contains("Monitoring failed") &&
                   button(window, "startMonitorButton")->isEnabled();
        }),
        "overlapping-root ownership was not rejected for GUI monitoring"
    );
    require(
        fake.monitor_count == 0 &&
            !text(window, "monitorStatus").contains("External Monitor") &&
            window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .contains("monitor status|trigger|stop") &&
            read_round(gates) == 1 &&
            monitor::request_control(
                owner.config.state_directory, monitor::ControlCommand::status
            )
                    .state == "synchronizing",
        "GUI silently attached another state directory or started a second "
        "engine"
    );
}

void test_external_replacement_during_confirmation() {
    Fixture fixture;
    auto lock = app::detail::acquire_runtime_lock(
        fixture.config.state_directory, "monitor"
    );
    auto server = std::make_unique<monitor::ControlServer>(
        fixture.config.state_directory, fixture.config.sync_data_directory
    );
    server->finish_round(exit_code::success);
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    window.show();
    require(
        wait_until([&] {
            return text(window, "monitorStatus").contains("External Monitor") &&
                   button(window, "startSyncButton")->isEnabled() &&
                   button(window, "stopSyncButton")->isEnabled();
        }),
        "external waiting Monitor did not become controllable"
    );
    bool confirmed = false;
    std::exception_ptr failure;
    QTimer::singleShot(0, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            try {
                server.reset();
                server = std::make_unique<monitor::ControlServer>(
                    fixture.config.state_directory,
                    fixture.config.sync_data_directory
                );
                server->finish_round(exit_code::success);
                confirmed = true;
            } catch (...) {
                failure = std::current_exception();
            }
            dialog->button(confirmed ? QMessageBox::Yes : QMessageBox::No)
                ->click();
        }
    });
    button(window, "stopSyncButton")->click();
    if (failure) {
        std::rethrow_exception(failure);
    }
    require(
        confirmed && wait_until([&] {
            return window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .contains("Monitor request failed:");
        }) &&
            !server->stop_token().stop_requested(),
        "a stop confirmation was inherited by a replacement Monitor"
    );
    require(
        wait_until([&] {
            return button(window, "startSyncButton")->isEnabled();
        }),
        "GUI did not recover after an instance mismatch"
    );
    static_cast<void>(monitor::request_control(
        fixture.config.state_directory, monitor::ControlCommand::trigger
    ));
    button(window, "startSyncButton")->click();
    require(
        wait_until([&] {
            return window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .contains("Monitor request merged;");
        }) &&
            server->take_trigger(),
        "GUI did not preserve merged-request acknowledgement semantics"
    );
    server->begin_round();
    server->finish_round(exit_code::failure);
    require(
        wait_until([&] {
            return text(window, "syncStatus").contains("Monitoring continues.");
        }),
        "an external failed round was hidden or treated as task termination"
    );
    server.reset();
    lock = {};
    require(
        wait_until([&] {
            return text(window, "syncStatus") == "Monitoring stopped." &&
                   button(window, "startMonitorButton")->isEnabled();
        }),
        "external shutdown did not release GUI actions after lock cleanup"
    );
}

} // namespace onedrive::test::gui_monitor
