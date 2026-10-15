#include "app/support.hpp"
#include "app/preflight.hpp"
#include "app/lock.hpp"
#include "app/checks.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "monitor_controller.hpp"
#include "sync_controller.hpp"
#include "onedrive/app/monitoring.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/logging/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <QApplication>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <fstream>
#include <sys/stat.h>
#include <thread>

namespace {
using namespace onedrive;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate> bool wait_until(Predicate predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 6000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return predicate();
}

struct Fixture {
    test::TemporaryDirectory temporary;
    std::filesystem::path file{temporary.path() / "config.toml"};
    config::Config config;
    account::DriveIdentity identity{test::test_drive_identity("me")};
    std::filesystem::path token;
    Fixture() {
        test::write_file(
            file,
            "config_version = 2\n[auth]\napplication_id = \"test-client\"\n"
            "[sync]\ndata_directory = \"" +
                (temporary.path() / "sync").string() +
                "\"\nmode = \"download_only\"\n[state]\ndirectory = \"" +
                (temporary.path() / "state").string() + "\"\n"
        );
        config = config::Config::load(file);
        app::detail::secure_state_directory(config.state_directory);
        token = account::AccountState::activate(
                    config.state_directory, identity, "saved-token"
                )
                    .token_directory /
                "refresh_token";
        std::filesystem::create_directories(config.sync_data_directory);
    }
};

class Observer final : public events::Observer {
    void on_event(const events::Event&) const override {
    }
};

QPushButton* button(QWidget& window, const char* name) {
    auto* found = window.findChild<QPushButton*>(name);
    require(found, "missing Monitor action");
    return found;
}

QString text(QWidget& window, const char* name) {
    auto* found = window.findChild<QLabel*>(name);
    require(found, "missing Monitor status");
    return found->text();
}

int read_round(const std::filesystem::path& directory) {
    std::ifstream input{directory / "round"};
    int count{};
    input >> count;
    return count;
}

class Child {
public:
    Child(const Fixture& fixture, const std::filesystem::path& gates) {
        process.start(
            QCoreApplication::applicationFilePath(),
            {"--monitor-child",
             QString::fromStdString(fixture.file.string()),
             QString::fromStdString(gates.string())}
        );
        require(
            process.waitForStarted(5000),
            "cannot start external Monitor subprocess"
        );
        require(
            wait_until([&] { return read_round(gates) == 1; }),
            "external Monitor did not enter its initial round"
        );
    }
    ~Child() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
    QProcess process;
};

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

void test_owned_window() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic_int rounds{0};
    std::atomic_bool release{false};
    std::atomic_bool cleanup{false};
    std::atomic_bool release_cleanup{false};
    int ordinary_syncs = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config, const events::Observer&, std::stop_token) {
            ++ordinary_syncs;
            return exit_code::success;
        },
        [&](config::Config config,
            const events::Observer& observer,
            std::stop_token token) {
            const auto root = account::AccountState::drive_data_directory(
                config.sync_data_directory, fixture.identity
            );
            const auto lease = app::detail::acquire_sync_directory_lock(
                root, config.state_directory, "monitor", observer
            );
            std::filesystem::create_directories(root);
            monitor::Monitor monitor{
                root,
                [&](const std::stop_token& sync_token) {
                    const auto round = ++rounds;
                    observer.sync_context(config.sync_mode, config.dry_run);
                    observer.sync_phase(events::SyncPhase::preparing);
                    observer.sync_phase(events::SyncPhase::uploading);
                    observer.upload_progress(
                        {.completed_files = 1,
                         .file_count = 1,
                         .uploaded = 100,
                         .total = 100,
                         .state = util::ProgressState::completed}
                    );
                    while ((round == 1 ? !release.load() : true) &&
                           !sync_token.stop_requested()) {
                        std::this_thread::sleep_for(5ms);
                    }
                    if (!sync_token.stop_requested()) {
                        observer.sync_summary({.uploaded = 1, .blocked = 1});
                        observer.sync_phase(events::SyncPhase::waiting);
                    }
                    return sync_token.stop_requested() ? exit_code::cancelled
                                                       : exit_code::blocked;
                },
                30s,
                20ms,
                {},
                config.state_directory
            };
            const auto result = monitor.run(token);
            cleanup = true;
            while (!release_cleanup) {
                std::this_thread::sleep_for(5ms);
            }
            return result;
        }
    };
    struct ReleaseCleanup {
        std::atomic_bool& release;
        ~ReleaseCleanup() {
            release = true;
        }
    } release_guard{release_cleanup};
    window.show();
    auto* start = button(window, "startMonitorButton");
    require(start->isEnabled(), "saved-account Monitor start was disabled");
    start->click();
    require(
        wait_until([&] {
            return rounds == 1 &&
                   text(window, "monitorStatus").contains("GUI-owned");
        }),
        "GUI-owned Monitor did not expose its ownership"
    );
    require(
        !button(window, "previewSyncButton")->isEnabled() &&
            !button(window, "startSyncButton")->isEnabled() &&
            !start->isEnabled(),
        "active Monitor permitted a concurrent GUI task"
    );
    require(
        !text(window, "syncStatus").contains("Synchronization completed"),
        "100 percent upload prematurely completed monitoring"
    );
    require(
        !button(window, "accountActionButton")->isEnabled() &&
            !button(window, "saveSettingsButton")->isEnabled(),
        "monitoring did not protect account/settings changes"
    );
    release = true;
    require(
        wait_until([&] {
            return button(window, "startSyncButton")->isEnabled();
        }),
        "waiting Monitor did not allow explicit triggering"
    );
    require(
        text(window, "syncStatus").contains("waiting for changes") &&
            text(window, "syncStatus").contains("Last round incomplete") &&
            text(window, "syncContext").contains("Automatic monitoring") &&
            text(window, "syncSummary").contains("1 uploaded") &&
            !window.findChild<QProgressBar*>("syncProgress")->isVisible(),
        "blocked round was mistaken for terminal Monitor completion"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return text(window, "syncStatus").contains("等待变化");
        }),
        "waiting Monitor did not retranslate"
    );
    localization.set_language(gui::Language::english);
    require(
        wait_until([&] {
            return text(window, "syncStatus").contains("waiting for changes");
        }),
        "waiting Monitor did not return to English"
    );
    button(window, "startSyncButton")->click();
    require(
        !button(window, "startSyncButton")->isEnabled(),
        "pending trigger remained enabled"
    );
    require(
        wait_until([&] { return rounds == 2; }) && ordinary_syncs == 0,
        "GUI trigger started another sync engine instead of requesting its "
        "Monitor"
    );
    button(window, "stopSyncButton")->click();
    require(
        wait_until([&] { return cleanup.load(); }) &&
            app::detail::inspect_runtime_owner(fixture.config.state_directory)
                .has_value(),
        "GUI stop declared cleanup complete too early"
    );
    require(
        text(window, "syncStatus").contains("Stopping") &&
            !button(window, "startMonitorButton")->isEnabled(),
        "stopping Monitor allowed a new task"
    );
    window.close();
    require(window.isVisible(), "window closed before owned Monitor cleanup");
    release_cleanup = true;
    require(
        wait_until([&] { return !window.isVisible(); }) &&
            !app::detail::inspect_runtime_owner(fixture.config.state_directory),
        "owned Monitor did not release locks before closing"
    );
}

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

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--monitor-child") {
            return child(argv[2], argv[3]);
        }
        QApplication application{argc, argv};
        application.setQuitOnLastWindowClosed(false);
        test_worker_exclusion_and_application_service();
        test_owned_window();
        test_external_window();
        test_overlapping_root_is_not_attached();
        test_readonly_discovery_and_unsafe_endpoint();
        test_controller_configuration_generations();
        test_controller_missing_target_and_stale_endpoint();
        test_readonly_owner_errors();
        test_external_replacement_during_confirmation();
        test_polling_reuses_selected_directory_without_config_logs();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
