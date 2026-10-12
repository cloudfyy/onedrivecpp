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
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTabWidget>

#include <spdlog/spdlog.h>

#include <atomic>
#include <limits>
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
            [&] { controller.invalidate_preview(); }, "during synchronization"
        ),
        "controller allowed UI invalidation while worker owns the cache"
    );
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

void test_default_controller_without_usable_credentials() {
    Fixture fixture;
    // Nonempty on disk passes preflight; token loading trims it to empty
    // and fails before any HTTP request.
    test::write_file(fixture.token, " \t\n");
    gui::SyncController controller;
    bool finished = false;
    QString detail;
    int result = exit_code::success;
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int code, const QString& error, bool preview) {
            require(preview, "default controller lost preview mode");
            result = code;
            detail = error;
            finished = true;
        }
    );
    controller.start(fixture.file, true);
    require(
        wait_until([&] { return finished; }) && result == exit_code::failure &&
            detail.contains("no saved Microsoft authentication is available") &&
            !controller.running(),
        "default production synchronization did not expose local credential "
        "failure"
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
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
    std::size_t expected_omitted = 0;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto previous_records = messages->document()->blockCount() -
                                      (expected_omitted != 0 ? 1 : 0);
        expected_omitted +=
            500 + static_cast<std::size_t>(previous_records) + 3;
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
                    QString{"Earlier events omitted: %1"}.arg(expected_omitted)
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
            "GUI history was unbounded or silently truncated"
        );
    }
}

void test_session_logs_and_tabs() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    int run = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config, const events::Observer& observer, std::stop_token) {
            ++run;
            for (int index = 0; index < 80; ++index) {
                observer.message(
                    events::MessageKind::information,
                    "info",
                    std::format("run {} info {}", run, index)
                );
            }
            observer.message(
                events::MessageKind::success, "success", "successful event %1"
            );
            observer.message(
                events::MessageKind::warning, "warning", "<b>raw warning %1</b>"
            );
            observer.message(
                events::MessageKind::error,
                "error",
                "raw failure\n/path/file %1"
            );
            observer.blocked_item(
                "blocked file", "conflict", "local conflict details"
            );
            return exit_code::success;
        }
    };
    window.show();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto* sync_page = window.findChild<QWidget*>("syncPage");
    auto* logs_page = window.findChild<QWidget*>("logsPage");
    auto* overview = window.findChild<QWidget*>("overviewPage");
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
    auto* level = window.findChild<QComboBox*>("logLevelFilter");
    auto* scroll_option = window.findChild<QCheckBox*>("logAutoScroll");
    auto* sync = button(window, "startSyncButton");
    auto* copy = button(window, "copyLogsButton");
    auto* clear = button(window, "clearLogsButton");
    require(
        tabs && tabs->count() == 4 && tabs->tabText(0) == "Overview" &&
            tabs->tabText(1) == "Synchronization" &&
            tabs->tabText(2) == "Logs" && tabs->tabText(3) == "Settings" &&
            tabs->currentWidget() == overview &&
            sync_page->isAncestorOf(sync) && !overview->isAncestorOf(sync) &&
            logs_page->isAncestorOf(messages) && messages->isReadOnly() &&
            scroll_option->isChecked(),
        "GUI tasks and logs were not separated into tabs"
    );
    auto* draft = window.findChild<QLineEdit*>("sync.data_directory");
    draft->setText("keep draft");
    require(
        !window.findChild<QPushButton*>("openSyncButton"),
        "overview retained an unnecessary synchronization button"
    );
    tabs->setCurrentWidget(sync_page);
    sync->click();
    tabs->setCurrentIndex(3);
    require(
        wait_until([&] { return sync->isEnabled(); }) &&
            tabs->currentIndex() == 3 && draft->text() == "keep draft" &&
            window.findChild<QLabel*>("overviewSyncStatus")->text() ==
                "Synchronization completed.",
        "tab switching reset the task, draft or overview summary"
    );
    button(window, "viewLogsButton")->click();
    QCoreApplication::processEvents();
    const auto first_history = messages->toPlainText();
    const QRegularExpression timestamp{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\])"
    };
    require(
        tabs->currentWidget() == logs_page &&
            timestamp.match(first_history).hasMatch() &&
            first_history.contains("[Synchronization] Info: run 1 info 0") &&
            !first_history.contains("saved-token") &&
            first_history.contains(
                "[Synchronization] Error: raw failure\n/path/file %1"
            ) &&
            messages->verticalScrollBar()->maximum() > 0 &&
            messages->verticalScrollBar()->value() ==
                messages->verticalScrollBar()->maximum(),
        "session logs lost timestamps, labels, raw details or auto-scroll"
    );
    scroll_option->setChecked(false);
    messages->verticalScrollBar()->setValue(0);
    button(window, "previewSyncButton")->click();
    require(
        wait_until([&] { return sync->isEnabled(); }) &&
            tabs->currentWidget() == logs_page && run == 2 &&
            messages->toPlainText().contains("run 1 info 0") &&
            messages->toPlainText().contains("[Preview] Info: run 2 info 0") &&
            messages->verticalScrollBar()->value() == 0,
        "new tasks cleared session history or ignored disabled auto-scroll"
    );
    scroll_option->setChecked(true);
    require(
        messages->verticalScrollBar()->value() ==
            messages->verticalScrollBar()->maximum(),
        "enabling auto-scroll did not scroll to the latest entry"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::warning))
    );
    const auto warnings = messages->toPlainText();
    require(
        warnings.contains("Warning: <b>raw warning %1</b>") &&
            warnings.contains("Blocked: blocked file [conflict]") &&
            !warnings.contains("raw failure") && !warnings.contains("info 0") &&
            !warnings.contains("successful event"),
        "warning filter leaked other log levels"
    );
    copy->click();
    require(
        QApplication::clipboard()->text() == warnings,
        "copy did not use the currently filtered log text"
    );
    const auto saved_timestamp = timestamp.match(warnings).captured();
    const int saved_blocks = messages->document()->blockCount();
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(
        tabs->currentWidget() == logs_page && tabs->tabText(2) == "日志" &&
            level->currentData().toInt() ==
                static_cast<int>(events::MessageKind::warning) &&
            level->currentText() == "警告" && copy->text() == "复制日志" &&
            clear->text() == "清空日志" &&
            scroll_option->text() == "自动滚动" &&
            messages->toPlainText().startsWith(saved_timestamp) &&
            messages->toPlainText().contains(
                "[同步] 警告：<b>raw warning %1</b>"
            ) &&
            messages->toPlainText().contains(
                "[预览] 警告：阻塞：blocked file [conflict]"
            ) &&
            messages->document()->blockCount() == saved_blocks &&
            draft->text() == "keep draft",
        "language changes lost filters, history, dates or drafts"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::error))
    );
    require(
        messages->toPlainText().contains("错误：raw failure\n/path/file %1") &&
            !messages->toPlainText().contains("raw warning"),
        "error filter was incorrect"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::success))
    );
    require(
        messages->toPlainText().contains("成功：successful event %1") &&
            messages->toPlainText().contains("预览完成。未修改同步文件。") &&
            !messages->toPlainText().contains("raw failure"),
        "success filter was incorrect"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::information))
    );
    require(
        messages->toPlainText().contains("run 1 info 0") &&
            !messages->toPlainText().contains("raw warning") &&
            !messages->toPlainText().contains("successful event"),
        "information filter was incorrect"
    );
    level->setCurrentIndex(0);
    localization.set_language(gui::Language::english);
    QCoreApplication::processEvents();
    const int before_reload = messages->document()->blockCount();
    button(window, "reloadSettingsButton")->click();
    require(
        messages->toPlainText().contains(
            "[Configuration] Success: Configuration reloaded."
        ) && messages->document()->blockCount() == before_reload + 1,
        "configuration reload was not logged or language switching duplicated "
        "entries"
    );
    clear->click();
    require(
        messages->toPlainText().isEmpty() && !copy->isEnabled() &&
            !clear->isEnabled() && sync->isEnabled() &&
            window.findChild<QLabel*>("syncStatus")->text() ==
                "Preview completed. No synchronized files were changed.",
        "clearing logs reset task results or left stale history"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::error))
    );
    sync->click();
    require(
        wait_until([&] { return sync->isEnabled(); }) && clear->isEnabled(),
        "new tasks did not resume logging after clear"
    );
    require(
        !messages->toPlainText().contains("run 1 info") &&
            messages->toPlainText().contains("raw failure"),
        "clear did not discard old session records"
    );
}

void test_controller_preview_cache() {
    for (const int invalidation : {0, 1, 2, 3, 4, 5}) {
        Fixture fixture;
        test::app::FakeRuntimeFactory implementation;
        implementation.use_real_item_store = true;
        const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
        const auto initial =
            test::app::FakeGraphClient{"me"}.list_delta(std::nullopt, {}, {});
        std::vector<std::optional<std::string>> requests;
        implementation.delta_override = [&](const auto& cursor,
                                            std::stop_token) {
            requests.push_back(cursor);
            if (cursor == initial.delta_link) {
                return graph::DeltaResult{
                    .changes = {},
                    .delta_link = "https://graph.example.test/after-preview",
                };
            }
            return initial;
        };
        gui::SyncController controller{[&](config::Config active,
                                           const events::Observer& observer,
                                           std::stop_token token,
                                           sync::PreviewCache* cache) {
            return app::synchronize_account(
                std::move(active), factory, observer, token, cache
            );
        }};
        bool finished = false;
        std::vector<std::string> messages;
        QObject::connect(
            &controller,
            &gui::SyncController::events_ready,
            [&](const auto& batch, std::size_t) {
                for (const auto& event : batch) {
                    if (const auto* message =
                            std::get_if<events::MessageEvent>(&event)) {
                        messages.push_back(message->event);
                    }
                }
            }
        );
        QObject::connect(
            &controller,
            &gui::SyncController::finished,
            [&](int result, const QString& error, bool) {
                require(
                    result == 0 && error.isEmpty(), "cached GUI task failed"
                );
                finished = true;
            }
        );
        const auto run = [&](bool preview) {
            finished = false;
            controller.start(fixture.file, preview);
            require(
                wait_until([&] { return finished; }),
                "cached task did not finish"
            );
        };
        const auto destination =
            account::AccountState::drive_data_directory(
                fixture.config.sync_data_directory, fixture.identity
            ) /
            "notes.txt";
        run(true);
        auto store =
            implementation.create_item_store(fixture.config, fixture.identity);
        store->open();
        require(
            requests == std::vector<std::optional<std::string>>{std::nullopt} &&
                !store->delta_link(fixture.identity.drive_id) &&
                store->drive_items(fixture.identity.drive_id).empty() &&
                !std::filesystem::exists(destination),
            "preview persisted its cursor, item state or files"
        );
        if (invalidation == 1) {
            test::write_file(
                fixture.file,
                test::read_file(fixture.file) + "\n# saved change\n"
            );
        } else if (invalidation == 2) {
            controller.invalidate_preview();
        } else if (invalidation == 3) {
            auto identity = fixture.identity;
            identity.user_id = "other-account";
            identity.user_display_name = "Other Account";
            implementation.identity_override = [identity](std::stop_token) {
                return identity;
            };
        } else if (invalidation == 4) {
            store->apply_delta({
                .drive_id = fixture.identity.drive_id,
                .delta_link = "https://graph.example.test/external-sync",
                .sync_filter_fingerprint = "external-filter",
            });
        } else if (invalidation == 5) {
            const auto alternate = fixture.root.path() / "alternate.toml";
            test::write_file(alternate, test::read_file(fixture.file));
            fixture.file = alternate;
        }
        store.reset();
        run(false);
        require(
            requests.size() == 2 &&
                requests.back() ==
                    (invalidation == 0
                         ? std::optional<std::string>{initial.delta_link}
                         : std::nullopt),
            "GUI repeated a full query or reused invalid "
            "configuration/account/state"
        );
        if (invalidation != 3) {
            store = implementation.create_item_store(
                fixture.config, fixture.identity
            );
            store->open();
            require(
                store->delta_link(fixture.identity.drive_id) ==
                        (invalidation == 0
                             ? "https://graph.example.test/after-preview"
                             : initial.delta_link) &&
                    store->drive_items(fixture.identity.drive_id).size() == 1 &&
                    test::read_file(destination) == std::string(42, 'x'),
                "cached sync did not apply retained metadata and latest cursor "
                "to SQLite"
            );
        }
        require(
            (std::ranges::find(messages, "preview_cache_reused") !=
             messages.end()) == (invalidation == 0),
            "GUI did not report the actual cache reuse decision"
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
        auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
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

void test_progress_phases() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic<int> step{};
    int runs = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config,
            const events::Observer& observer,
            std::stop_token token) {
            const bool report_completion = ++runs == 1;
            const auto wait_for = [&](int target) {
                while (step < target && !token.stop_requested()) {
                    QThread::msleep(1);
                }
                return !token.stop_requested();
            };
            for (int stage = 0; stage <= 6; ++stage) {
                if (!wait_for(stage)) {
                    return exit_code::cancelled;
                }
                switch (stage) {
                case 0:
                    observer.delta_progress(
                        11, 2569, util::ProgressState::ongoing
                    );
                    break;
                case 1:
                    if (report_completion) {
                        observer.delta_progress(
                            11, 2569, util::ProgressState::completed
                        );
                    } else {
                        observer.delta_summary(
                            {.pages = 11, .scanned_items = 2569}
                        );
                    }
                    break;
                case 2:
                    observer.download_progress(
                        0, 0, 0, 0, util::ProgressState::ongoing
                    );
                    break;
                case 3:
                    observer.download_progress(
                        0, 2, 0, 100, util::ProgressState::ongoing
                    );
                    observer.end_download_progress();
                    break;
                case 4:
                    observer.download_progress(
                        1, 2, 0, 0, util::ProgressState::ongoing
                    );
                    observer.end_download_progress();
                    break;
                case 5:
                    observer.download_progress(
                        1,
                        2,
                        50,
                        100,
                        util::ProgressState::ongoing,
                        {.bytes_per_second = 25,
                         .estimated_seconds_remaining = 2}
                    );
                    observer.end_download_progress();
                    break;
                case 6:
                    observer.download_progress(
                        2,
                        2,
                        100,
                        100,
                        util::ProgressState::completed,
                        {.bytes_per_second = 25,
                         .estimated_seconds_remaining = 0}
                    );
                    observer.end_download_progress();
                    break;
                }
            }
            return wait_for(7) ? exit_code::success : exit_code::cancelled;
        }
    };
    auto* sync = button(window, "startSyncButton");
    auto* progress = window.findChild<QProgressBar*>("syncProgress");
    auto* hint = window.findChild<QLabel*>("syncProgressHint");
    auto* status = window.findChild<QLabel*>("syncStatus");
    auto* transfer = window.findChild<QLabel*>("transferStatus");
    for (int attempt = 0; attempt < 2; ++attempt) {
        step = 0;
        sync->click();
        require(
            progress->isHidden() && progress->maximum() == 100 &&
                !hint->isHidden() && progress->value() == 0,
            "preparation showed a misleading percentage or stale progress"
        );
        require(
            wait_until([&] {
                return status->text() ==
                       "Scanning remote changes: 11 pages, 2,569 items.";
            }) &&
                progress->isHidden() &&
                hint->text() == "Remote totals are unknown; a percentage "
                                "cannot be calculated.",
            "remote scanning showed a full/busy bar despite unknown totals"
        );
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            progress->isHidden() &&
                hint->text() == "远端总量未知，无法计算百分比。",
            "unknown scan totals did not translate without changing progress"
        );
        localization.set_language(gui::Language::english);
        QCoreApplication::processEvents();
        step = 1;
        require(
            wait_until([&] { return status->text() == "Synchronizing..."; }) &&
                progress->isHidden() && !sync->isEnabled(),
            "completed scan or delta summary left the GUI scanning"
        );
        step = 2;
        require(
            wait_until([&] {
                return hint->text() == "Download totals are unknown; a "
                                       "percentage cannot be calculated.";
            }) &&
                status->text() == "Downloading files..." &&
                progress->isHidden(),
            "unknown download totals were presented as measurable progress"
        );
        step = 3;
        require(
            wait_until([&] { return !progress->isHidden(); }) &&
                progress->maximum() == 100 && progress->value() == 0 &&
                hint->isHidden() && status->text() == "Downloading files..." &&
                progress->text() == "Downloads: 0%",
            "terminal newline reset download progress to an indeterminate phase"
        );
        step = 4;
        require(
            wait_until([&] { return progress->value() == 50; }) &&
                !progress->isHidden() &&
                transfer->text().contains("1 / 2 files; 0 B / 0 B"),
            "zero-byte downloads did not use measurable file-count progress"
        );
        step = 5;
        require(
            wait_until([&] {
                return transfer->text().contains("50 B / 100 B");
            }) &&
                progress->value() == 50 && progress->text() == "Downloads: 50%",
            "byte progress or terminal newline handling was incorrect"
        );
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            progress->value() == 50 && progress->text() == "下载：50%" &&
                progress->accessibleName() == "下载进度",
            "download percentage changed or lost its label during translation"
        );
        localization.set_language(gui::Language::english);
        QCoreApplication::processEvents();
        step = 6;
        require(
            wait_until([&] { return status->text() == "Synchronizing..."; }) &&
                progress->isHidden() && !hint->isHidden() &&
                !sync->isEnabled() &&
                transfer->text().contains("2 / 2 files; 100 B / 100 B") &&
                !transfer->text().contains("remaining"),
            "finished downloads falsely reported overall completion or a stale "
            "ETA"
        );
        step = 7;
        require(
            wait_until([&] { return sync->isEnabled(); }) &&
                status->text() == "Synchronization completed." &&
                !progress->isHidden() && progress->value() == 100 &&
                progress->text() == "100%" && hint->isHidden(),
            "overall 100 percent appeared before task completion"
        );
    }
}

void test_readable_transfer_numbers() {
    struct Example {
        std::uint64_t downloaded;
        std::uint64_t total;
        std::uint64_t speed;
        std::optional<std::uint64_t> seconds;
        QString amounts;
        QString english_time;
        QString chinese_time;
    };
    const Example examples[]{
        {0, 0, 0, std::nullopt, "0 B / 0 B; 0 B", {}, {}},
        {1023, 1024, 0, 0, "1,023 B / 1.0 KiB; 0 B", "0 s", "0 秒"},
        {1024,
         1ULL << 20,
         1024,
         59,
         "1.0 KiB / 1.0 MiB; 1.0 KiB",
         "59 s",
         "59 秒"},
        {1ULL << 20,
         1ULL << 30,
         1ULL << 20,
         60,
         "1.0 MiB / 1.0 GiB; 1.0 MiB",
         "1 min 0 s",
         "1 分钟 0 秒"},
        {1ULL << 30,
         1ULL << 40,
         1ULL << 30,
         3599,
         "1.0 GiB / 1.0 TiB; 1.0 GiB",
         "59 min 59 s",
         "59 分钟 59 秒"},
        {1ULL << 40,
         1ULL << 50,
         1ULL << 40,
         3600,
         "1.0 TiB / 1.0 PiB; 1.0 TiB",
         "1 h 0 min",
         "1 小时 0 分钟"},
        {1ULL << 50,
         1ULL << 60,
         1ULL << 50,
         86399,
         "1.0 PiB / 1.0 EiB; 1.0 PiB",
         "23 h 59 min",
         "23 小时 59 分钟"},
        {1ULL << 60,
         std::numeric_limits<std::uint64_t>::max(),
         1ULL << 60,
         86400,
         "1.0 EiB / 16.0 EiB; 1.0 EiB",
         "1 d 0 h",
         "1 天 0 小时"},
        {30082294,
         17349245011,
         126523,
         136886,
         "28.7 MiB / 16.2 GiB; 123.6 KiB",
         "1 d 14 h",
         "1 天 14 小时"},
    };
    struct RestoreLocale {
        QLocale original;
        ~RestoreLocale() {
            QLocale::setDefault(original);
        }
    } restore{QLocale{}};
    QLocale::setDefault(QLocale{QLocale::German});
    for (const auto& example : examples) {
        Fixture fixture;
        gui::Localization localization{gui::Language::english};
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            &localization,
            [&example](
                config::Config,
                const events::Observer& observer,
                std::stop_token token
            ) {
                observer.download_progress(
                    64,
                    13341,
                    example.downloaded,
                    example.total,
                    util::ProgressState::ongoing,
                    {.bytes_per_second = example.speed,
                     .estimated_seconds_remaining = example.seconds}
                );
                while (!token.stop_requested()) {
                    QThread::msleep(1);
                }
                return exit_code::cancelled;
            }
        };
        auto* transfer = window.findChild<QLabel*>("transferStatus");
        button(window, "startSyncButton")->click();
        auto english =
            "Downloads: 64 / 13,341 files; " + example.amounts + "/s";
        if (example.seconds) {
            english += "; about " + example.english_time + " remaining";
        }
        require(
            wait_until([&] { return transfer->text() == english; }),
            "transfer numbers did not use readable units and selected locale"
        );
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        auto chinese = "下载：64 / 13,341 个文件；" + example.amounts + "/秒";
        chinese.replace("; ", "；");
        if (example.seconds) {
            chinese += "；预计剩余 " + example.chinese_time;
        }
        if (transfer->text() != chinese) {
            throw std::runtime_error{("translated transfer: " +
                                      transfer->text() +
                                      "; expected: " + chinese)
                                         .toStdString()};
        }
        localization.set_language(gui::Language::english);
        QCoreApplication::processEvents();
        require(
            transfer->text() == english,
            "language switching changed the underlying transfer values"
        );
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
            observer.delta_summary({.pages = 99, .scanned_items = 999});
            observer.end_download_progress();
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
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
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
            transfer->text().contains("1 / 2 files; 50 B / 100 B; 25 B/s") &&
            transfer->text().contains("2 s remaining"),
        "progress or task interlocks were incorrect"
    );
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        QCoreApplication::processEvents();
        require(
            calls == 1 && progress->value() == 50 && !sync->isEnabled() &&
                stop->isEnabled() && !late_progress,
            "changing tabs interrupted an active task"
        );
    }
    tabs->setCurrentWidget(window.findChild<QWidget*>("logsPage"));
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
    button(window, "clearLogsButton")->click();
    require(
        messages->toPlainText().isEmpty() && progress->value() == 50 &&
            calls == 1 && !sync->isEnabled() &&
            draft->text() == "unsaved draft",
        "clearing logs changed an active synchronization task"
    );
    stop->click();
    require(
        wait_until([&] { return late_progress.load(); }),
        "cancelled task did not send its late progress"
    );
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
        test_default_controller_without_usable_credentials();
        test_controller_lifecycle();
        test_history_bound();
        test_session_logs_and_tabs();
        test_controller_preview_cache();
        test_preview_and_apply();
        test_progress_phases();
        test_readable_transfer_numbers();
        test_progress_and_stop();
        test_results_and_preflight();
        test_close_and_availability();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
