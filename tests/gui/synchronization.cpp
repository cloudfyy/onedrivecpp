#include "app/support.hpp"
#include "app/preflight.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "sync_controller.hpp"
#include "onedrive/app/synchronization.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>

#include <spdlog/spdlog.h>

#include <atomic>
#include <thread>

namespace {

using namespace onedrive;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

template <typename Predicate> bool wait_until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

struct Fixture {
    test::TemporaryDirectory root;
    std::filesystem::path file{root.path() / "config.toml"};
    config::Config config;
    account::DriveIdentity identity{test::test_drive_identity("me")};
    std::filesystem::path token;

    explicit Fixture(bool dry_run = false) {
        test::write_file(
            file,
            "config_version = 2\n[sync]\ndata_directory = \"" +
                (root.path() / "sync").string() +
                "\"\nupload = false\ndry_run = " +
                (dry_run ? "true" : "false") + "\n[state]\ndirectory = \"" +
                (root.path() / "state").string() +
                "\"\n[auth]\napplication_id = \"test-client\"\n"
        );
        config = config::Config::load(file);
        std::filesystem::create_directories(config.state_directory);
        std::filesystem::permissions(
            config.state_directory, std::filesystem::perms::owner_all
        );
        token = account::AccountState::activate(
                    config.state_directory, identity, "saved-token"
                )
                    .token_directory /
                "refresh_token";
        std::filesystem::create_directories(config.sync_data_directory);
        test::write_file(
            config.sync_data_directory / "keep.txt", "keep this file"
        );
    }
};

QPushButton* button(QWidget& window, const char* name) {
    auto* result = window.findChild<QPushButton*>(name);
    require(result != nullptr, "missing synchronization control");
    return result;
}

void test_controller() {
    Fixture fixture;
    const auto ui_thread = std::this_thread::get_id();
    const auto logger = spdlog::default_logger();
    std::atomic<bool> worker_done{};
    bool worker_valid = false;
    gui::SyncController controller{[&](config::Config active,
                                       const events::Observer& observer,
                                       std::stop_token) {
        worker_valid = std::this_thread::get_id() != ui_thread &&
                       active.dry_run && !active.force_large_delete &&
                       test::throws_with<std::runtime_error>(
                           [&] {
                               const app::detail::RuntimePreflight busy{
                                   active, app::detail::Operation::logout
                               };
                           },
                           "already using this state directory"
                       );
        for (int index = 0; index < 1000; ++index) {
            observer.message(
                events::MessageKind::information, "test", std::to_string(index)
            );
        }
        for (std::uint64_t index = 0; index < 10000; ++index) {
            observer.download_progress(
                0, 1, index, 10000, util::ProgressState::ongoing
            );
        }
        worker_done = true;
        return exit_code::success;
    }};
    bool finished = false;
    bool delivered_on_ui = true;
    std::size_t count = 0;
    std::size_t omitted = 0;
    std::uint64_t latest_progress = 0;
    QObject::connect(
        &controller,
        &gui::SyncController::events_ready,
        [&](const auto& batch, std::size_t dropped) {
            delivered_on_ui =
                delivered_on_ui && std::this_thread::get_id() == ui_thread;
            count += batch.size();
            omitted += dropped;
            for (const auto& event : batch) {
                if (const auto* progress =
                        std::get_if<events::DownloadProgressEvent>(&event)) {
                    latest_progress = progress->downloaded;
                }
            }
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool preview) {
            require(
                result == exit_code::success && error.isEmpty() && preview &&
                    !controller.running(),
                "controller returned an incorrect result"
            );
            finished = true;
        }
    );
    controller.start(fixture.file, true);
    require(
        test::throws_with<std::logic_error>(
            [&] { controller.start(fixture.file, false); }, "already running"
        ),
        "controller accepted concurrent tasks"
    );
    QElapsedTimer timer;
    timer.start();
    while (!worker_done && timer.elapsed() < 5000) {
        QThread::msleep(1);
    }
    require(
        worker_done && wait_until([&] { return finished; }),
        "controller did not finish its background task"
    );
    require(
        worker_valid && delivered_on_ui && count == 500 && omitted == 501 &&
            latest_progress == 9999 && spdlog::default_logger() == logger,
        "controller lost its lock, event bound, latest progress or logger"
    );
}

void test_controller_lifecycle() {
    Fixture fixture;
    std::atomic<int> entered{};
    auto synchronize =
        [&](config::Config, const events::Observer&, std::stop_token token) {
            ++entered;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        };
    gui::SyncController controller{synchronize};
    int completed = 0;
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool) {
            require(
                result == exit_code::cancelled && error.isEmpty(),
                "controller cancellation was reported as failure"
            );
            ++completed;
        }
    );
    for (int attempt = 1; attempt <= 2; ++attempt) {
        controller.start(fixture.file, false);
        require(
            wait_until([&] { return entered == attempt; }),
            "restarted task did not run"
        );
        controller.cancel();
        require(
            wait_until([&] { return completed == attempt; }) &&
                !controller.running(),
            "controller did not join a cancelled task before restart"
        );
        const app::detail::RuntimePreflight unlocked{
            fixture.config, app::detail::Operation::logout
        };
    }
    controller.start(fixture.file, false);
    controller.cancel();
    require(
        wait_until([&] { return completed == 3; }),
        "immediate cancellation did not finish"
    );
    const auto expected = entered + 1;
    const auto logger = spdlog::default_logger();
    {
        gui::SyncController destroyed{synchronize};
        destroyed.start(fixture.file, false);
        require(
            wait_until([&] { return entered == expected; }),
            "destructor test did not enter its task"
        );
    }
    QCoreApplication::processEvents();
    require(
        spdlog::default_logger() == logger,
        "destructor did not restore the logger"
    );
    const app::detail::RuntimePreflight unlocked{
        fixture.config, app::detail::Operation::logout
    };
}

void test_history_bound() {
    Fixture fixture;
    std::atomic<int> run{};
    std::atomic<bool> emitted{};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config, const events::Observer& observer, std::stop_token) {
            ++run;
            for (int index = 0; index < 1000; ++index) {
                observer.message(
                    events::MessageKind::information,
                    "test",
                    std::format("run {} event {}", run.load(), index)
                );
            }
            emitted = true;
            return exit_code::success;
        }
    };
    auto* sync = button(window, "startSyncButton");
    auto* messages = window.findChild<QPlainTextEdit*>("syncMessages");
    for (int attempt = 1; attempt <= 2; ++attempt) {
        emitted = false;
        sync->click();
        QElapsedTimer timer;
        timer.start();
        while (!emitted && timer.elapsed() < 5000) {
            QThread::msleep(1);
        }
        require(
            emitted && wait_until([&] { return sync->isEnabled(); }) &&
                messages->toPlainText().startsWith(
                    "Earlier events omitted: 500"
                ) &&
                messages->toPlainText().contains(
                    QString{"run %1 event 999"}.arg(attempt)
                ) &&
                !messages->toPlainText().contains(
                    QString{"run %1 event 0"}.arg(attempt)
                ) &&
                (attempt == 1 ||
                 !messages->toPlainText().contains("run 1 event")) &&
                messages->document()->blockCount() == 501,
            "GUI history was unbounded, silently truncated or retained a "
            "previous task"
        );
    }
}

void test_preview_and_apply() {
    for (const bool persisted_preview : {false, true}) {
        Fixture fixture{persisted_preview};
        const auto source = test::read_file(fixture.file);
        test::app::FakeRuntimeFactory implementation;
        const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            nullptr,
            [&](config::Config active,
                const events::Observer& observer,
                std::stop_token token) {
                return app::synchronize_account(
                    std::move(active), factory, observer, token
                );
            }
        };
        auto* preview = button(window, "previewSyncButton");
        auto* sync = button(window, "startSyncButton");
        auto* status = window.findChild<QLabel*>("syncStatus");
        auto* progress = window.findChild<QProgressBar*>("syncProgress");
        auto* messages = window.findChild<QPlainTextEdit*>("syncMessages");
        auto* draft = window.findChild<QLineEdit*>("sync.data_directory");
        draft->setText("unsaved draft");
        const auto destination =
            account::AccountState::drive_data_directory(
                fixture.config.sync_data_directory, fixture.identity
            ) /
            "notes.txt";
        require(
            preview->isEnabled() && sync->isEnabled(),
            "saved account could not synchronize"
        );
        preview->click();
        require(
            wait_until([&] { return preview->isEnabled(); }) &&
                status->text() ==
                    "Preview completed. No synchronized files were changed." &&
                progress->value() == 100 &&
                !std::filesystem::exists(destination) &&
                test::read_file(
                    fixture.config.sync_data_directory / "keep.txt"
                ) == "keep this file" &&
                test::read_file(fixture.file) == source &&
                messages->toPlainText().contains("Dry run") &&
                draft->text() == "unsaved draft",
            "preview changed files, settings or lost its plan"
        );
        sync->click();
        require(
            wait_until([&] { return sync->isEnabled(); }) &&
                status->text() == (persisted_preview
                                       ? "Preview completed. No synchronized "
                                         "files were changed."
                                       : "Synchronization completed.") &&
                std::filesystem::exists(destination) == !persisted_preview &&
                test::read_file(fixture.file) == source &&
                draft->text() == "unsaved draft",
            "GUI synchronization ignored dry-run configuration or its saved "
            "settings"
        );
        if (!persisted_preview) {
            require(
                test::read_file(destination) == std::string(42, 'x'),
                "GUI service did not write the expected downloaded file"
            );
        }
    }
}

void test_progress_and_stop() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic<int> calls{};
    std::atomic<bool> finish{};
    std::atomic<bool> late_progress{};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config,
            const events::Observer& observer,
            std::stop_token token) {
            ++calls;
            observer.delta_progress(2, 7, util::ProgressState::ongoing);
            observer.delta_summary(
                {.files = 2, .directories = 1, .deletions = 1}
            );
            observer.message(
                events::MessageKind::warning, "warning", "backend detail %1"
            );
            observer.section(
                "plan",
                "Plan:",
                {{.label = "downloads:", .key = "downloads", .value = "2"}}
            );
            observer.blocked_item(
                "blocked.txt", "conflict", "keep local content"
            );
            observer.download_progress(
                1,
                2,
                50,
                100,
                util::ProgressState::ongoing,
                {.bytes_per_second = 25, .estimated_seconds_remaining = 2}
            );
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            observer.delta_progress(99, 999, util::ProgressState::ongoing);
            late_progress = true;
            while (!finish) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        }
    };
    struct FinishWorker {
        std::atomic<bool>& finish;
        ~FinishWorker() {
            finish = true;
        }
    } release{finish};
    auto* sync = button(window, "startSyncButton");
    auto* stop = button(window, "stopSyncButton");
    auto* progress = window.findChild<QProgressBar*>("syncProgress");
    auto* status = window.findChild<QLabel*>("syncStatus");
    auto* messages = window.findChild<QPlainTextEdit*>("syncMessages");
    auto* transfer = window.findChild<QLabel*>("transferStatus");
    const auto source = test::read_file(fixture.file);
    auto* draft = window.findChild<QLineEdit*>("sync.data_directory");
    draft->setText("unsaved draft");
    sync->click();
    require(
        wait_until([&] { return progress->value() == 50; }) &&
            !sync->isEnabled() &&
            !button(window, "previewSyncButton")->isEnabled() &&
            stop->isEnabled() &&
            !button(window, "accountActionButton")->isEnabled() &&
            !window.findChild<QWidget*>("settingsPage")->isEnabled() &&
            messages->toPlainText().contains(
                "Blocked: blocked.txt [conflict]"
            ) &&
            messages->toPlainText().contains("downloads: 2") &&
            transfer->text().contains("1/2 files; 50/100 bytes; 25 B/s") &&
            transfer->text().contains("2 seconds remaining"),
        "progress or task interlocks were incorrect"
    );
    Q_EMIT sync->clicked();
    Q_EMIT button(window, "previewSyncButton")->clicked();
    Q_EMIT button(window, "signInButton")->clicked();
    Q_EMIT button(window, "accountActionButton")->clicked();
    Q_EMIT button(window, "saveSettingsButton")->clicked();
    Q_EMIT button(window, "reloadSettingsButton")->clicked();
    Q_EMIT button(window, "importSettingsButton")->clicked();
    require(
        calls == 1 && test::read_file(fixture.token) == "saved-token" &&
            test::read_file(fixture.file) == source &&
            draft->text() == "unsaved draft" &&
            !QApplication::activeModalWidget(),
        "disabled account or configuration actions changed an active task"
    );
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(
        sync->text() == "立即同步" && stop->text() == "停止" &&
            status->text() == "正在下载文件…" && progress->value() == 50 &&
            messages->toPlainText().contains(
                "远端变更：2 个文件，1 个文件夹，1 项删除。"
            ) &&
            messages->toPlainText().contains("警告：backend detail %1") &&
            messages->toPlainText().contains("阻塞：blocked.txt [conflict]") &&
            draft->text() == "unsaved draft",
        "language switching reset progress, events or settings drafts"
    );
    stop->click();
    require(wait_until([&] { return late_progress.load(); }),
            "cancelled task did not send its late progress");
    QCoreApplication::processEvents();
    require(
        !stop->isEnabled() && status->text() == "正在停止，等待任务结束…",
        "late progress overrode the stopping state"
    );
    finish = true;
    require(
        wait_until([&] { return sync->isEnabled(); }) &&
            status->text() == "同步已取消。" && !stop->isEnabled() &&
            button(window, "accountActionButton")->isEnabled() &&
            window.findChild<QWidget*>("settingsPage")->isEnabled() &&
            progress->maximum() == 100 && progress->value() == 50,
        "cancelled synchronization did not restore controls"
    );
}

void test_results_and_preflight() {
    for (const int outcome : {exit_code::blocked, exit_code::failure, 42, -1}) {
        Fixture fixture;
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            nullptr,
            [outcome](
                config::Config, const events::Observer&, std::stop_token
            ) {
                if (outcome == -1) {
                    throw std::runtime_error{"test failure %1"};
                }
                return outcome;
            }
        };
        auto* sync = button(window, "startSyncButton");
        auto* status = window.findChild<QLabel*>("syncStatus");
        sync->click();
        require(
            wait_until([&] { return sync->isEnabled(); }),
            "failed task did not finish"
        );
        require(
            outcome == exit_code::blocked
                ? status->text() ==
                      "Synchronization incomplete: some items were blocked."
            : outcome == -1
                ? status->text() == "Synchronization failed: test failure %1"
                : status->text().contains(QString{"exit code %1"}.arg(outcome)),
            "GUI reported a failed or blocked task as successful"
        );
    }
    Fixture fixture;
    int calls = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config, const events::Observer&, std::stop_token) {
            ++calls;
            return exit_code::success;
        }
    };
    auto* preview = button(window, "previewSyncButton");
    {
        const app::detail::RuntimePreflight busy{
            fixture.config, app::detail::Operation::logout
        };
        preview->click();
        require(
            wait_until([&] { return preview->isEnabled(); }) && calls == 0 &&
                window.findChild<QLabel*>("syncStatus")
                    ->text()
                    .contains("another onedrive-cpp process"),
            "GUI preview bypassed the runtime lock"
        );
    }
    test::write_file(fixture.file, "[broken TOML");
    preview->click();
    require(
        wait_until([&] { return preview->isEnabled(); }) && calls == 0 &&
            window.findChild<QLabel*>("syncStatus")
                ->text()
                .startsWith("Preview failed: "),
        "GUI preview ignored a configuration load failure"
    );
}

void test_close_and_availability() {
    Fixture fixture;
    {
        auto state = gui::load_app_state_view_model(fixture.file);
        state.sync_error = "invalid sync path";
        gui::MainWindow window{state};
        require(
            !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "previewSyncButton")->isEnabled() &&
                !button(window, "stopSyncButton")->isEnabled(),
            "invalid synchronization paths enabled a task"
        );
    }
    {
        auto state = gui::load_app_state_view_model(fixture.file);
        state.account_status = gui::AccountStatus::no_credentials;
        gui::MainWindow window{state};
        require(
            !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "previewSyncButton")->isEnabled(),
            "signed-out accounts enabled synchronization"
        );
    }
    std::atomic<bool> entered{};
    std::atomic<bool> finish{};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config, const events::Observer&, std::stop_token token) {
            entered = true;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            while (!finish) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        }
    };
    struct FinishWorker {
        std::atomic<bool>& finish;
        ~FinishWorker() {
            finish = true;
        }
    } release{finish};
    window.show();
    button(window, "startSyncButton")->click();
    require(
        wait_until([&] { return entered.load(); }),
        "close test did not start a task"
    );
    window.close();
    require(
        window.isVisible() && !button(window, "stopSyncButton")->isEnabled(),
        "window closed without waiting for synchronization"
    );
    finish = true;
    require(
        wait_until([&] { return !window.isVisible(); }),
        "window did not join its task before closing"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    application.setQuitOnLastWindowClosed(false);
    gui::Localization localization{gui::Language::english};
    try {
        test_controller();
        test_controller_lifecycle();
        test_history_bound();
        test_preview_and_apply();
        test_progress_and_stop();
        test_results_and_preflight();
        test_close_and_availability();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
