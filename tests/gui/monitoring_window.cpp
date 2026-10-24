#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

namespace onedrive::test::gui_monitor {

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
                        observer.message(
                            events::MessageKind::information,
                            "local_item_removed",
                            "Removed remotely deleted local item 'Root "
                            "document (version 2).pdf'."
                        );
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
        window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .count("Starting GUI-owned monitoring...") == 1,
        "GUI-owned Monitor startup was logged more than once"
    );
    require(
        text(window, "syncStatus").contains("waiting for changes") &&
            text(window, "syncStatus").contains("Last round incomplete") &&
            text(window, "syncContext").contains("Automatic monitoring") &&
            text(window, "syncSummary").contains("1 uploaded") &&
            window.findChild<QPlainTextEdit*>("logMessages")
                ->toPlainText()
                .contains(
                    "Removed remotely deleted local item 'Root document "
                    "(version 2).pdf'."
                ) &&
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
    require(
        window.findChild<QPlainTextEdit*>("logMessages")
            ->toPlainText()
            .contains(
                "Removed remotely deleted local item 'Root document (version "
                "2).pdf'."
            ),
        "starting another Monitor round erased the prior deletion log"
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

void test_monitor_session_totals() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic_int requested{-1};
    std::atomic_int released{-1};
    std::atomic_int preparing{-1};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [](config::Config, const events::Observer& observer, std::stop_token) {
            observer.sync_summary({.downloaded = 9});
            return exit_code::success;
        },
        [&](config::Config,
            const events::Observer& observer,
            std::stop_token token) {
            const events::SyncSummaryEvent rounds[]{
                {.downloaded = 1,
                 .uploaded = 2,
                 .moved = 3,
                 .deleted = 4,
                 .blocked = 47},
                {.blocked = 47},
                {.downloaded = 5,
                 .uploaded = 6,
                 .moved = 7,
                 .deleted = 8,
                 .blocked = 3},
            };
            for (int round = 0; !token.stop_requested(); ++round) {
                while (requested < round && !token.stop_requested()) {
                    std::this_thread::sleep_for(2ms);
                }
                if (token.stop_requested()) {
                    break;
                }
                observer.sync_phase(events::SyncPhase::preparing);
                preparing = round;
                while (released < round && !token.stop_requested()) {
                    std::this_thread::sleep_for(2ms);
                }
                if (token.stop_requested()) {
                    break;
                }
                if (round == 3) {
                    throw std::runtime_error{"round failed without a summary"};
                }
                observer.sync_summary(rounds[round]);
                observer.sync_phase(events::SyncPhase::waiting);
            }
            return exit_code::cancelled;
        }
    };
    window.show();
    auto* start = button(window, "startMonitorButton");
    const auto initial =
        "Monitor session totals: 0 downloaded, 0 uploaded, 0 moved, 0 deleted.";
    start->click();
    require(
        wait_until([&] { return text(window, "monitorTotals") == initial; }) &&
            text(window, "syncSummary").isEmpty(),
        "new Monitor session inherited an earlier total or completion time"
    );
    const auto finish_round = [&](int round, const QString& expected) {
        requested = round;
        require(
            wait_until([&] {
                return preparing == round &&
                       text(window, "syncStatus")
                           .contains("Preparing synchronization");
            }),
            "Monitor did not enter the requested round"
        );
        released = round;
        require(
            wait_until([&] {
                return text(window, "syncStatus")
                           .contains("waiting for changes") &&
                       text(window, "syncSummary").startsWith(expected);
            }),
            "Monitor lost its completed-round statistics"
        );
    };
    finish_round(
        0,
        "Last round totals: 1 downloaded, 2 uploaded, 3 moved, 4 deleted, 47 "
        "blocked."
    );
    const auto first_totals = text(window, "monitorTotals");
    require(
        first_totals == "Monitor session totals: 1 downloaded, 2 uploaded, 3 "
                        "moved, 4 deleted.",
        "Monitor did not accumulate all four operation counts"
    );
    finish_round(
        1,
        "Last round totals: 0 downloaded, 0 uploaded, 0 moved, 0 deleted, 47 "
        "blocked."
    );
    require(
        text(window, "monitorTotals") == first_totals,
        "an empty follow-up round erased successful session operations"
    );
    finish_round(
        2,
        "Last round totals: 5 downloaded, 6 uploaded, 7 moved, 8 deleted, 3 "
        "blocked."
    );
    const auto total = text(window, "monitorTotals");
    const auto last = text(window, "syncSummary");
    require(
        total == "Monitor session totals: 6 downloaded, 8 uploaded, 10 moved, "
                 "12 deleted." &&
            !total.contains("blocked") && !last.contains("97 blocked"),
        "Monitor totals did not sum operations or accumulated blockers"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return text(window, "monitorTotals") ==
                       "本次监控累计：下载 6，上传 8，移动 10，删除 12。" &&
                   text(window, "syncSummary")
                       .startsWith(
                           "最近一轮：下载 5，上传 6，移动 7，删除 8，阻止 3。"
                       );
        }),
        "Monitor totals did not retranslate without resetting the counters"
    );
    localization.set_language(gui::Language::english);
    require(
        wait_until([&] {
            return text(window, "monitorTotals") == total &&
                   text(window, "syncSummary") == last;
        }),
        "switching back to English changed Monitor statistics"
    );
    button(window, "clearLogsButton")->click();
    require(
        text(window, "monitorTotals") == total &&
            text(window, "syncSummary") == last,
        "language changes or clearing logs changed Monitor statistics"
    );
    requested = 3;
    require(
        wait_until([&] { return preparing == 3; }) &&
            text(window, "monitorTotals") == total &&
            text(window, "syncSummary") == last,
        "starting a round cleared the latest completed-round statistics"
    );
    released = 3;
    require(
        wait_until([&] { return start->isEnabled(); }) &&
            text(window, "syncStatus").contains("Monitoring failed") &&
            text(window, "monitorTotals") == total &&
            text(window, "syncSummary") == last,
        "an unreported failed round erased or added statistics"
    );
    requested = -1;
    released = -1;
    preparing = -1;
    start->click();
    require(
        wait_until([&] { return text(window, "monitorTotals") == initial; }) &&
            text(window, "syncSummary").isEmpty(),
        "restarting Monitor did not reset the session totals and timestamp"
    );
    finish_round(
        0,
        "Last round totals: 1 downloaded, 2 uploaded, 3 moved, 4 deleted, 47 "
        "blocked."
    );
    button(window, "stopSyncButton")->click();
    require(
        wait_until([&] { return start->isEnabled(); }) &&
            text(window, "monitorTotals") == first_totals &&
            text(window, "syncSummary").contains("47 blocked"),
        "cancelling Monitor lost completed session statistics"
    );
    button(window, "startSyncButton")->click();
    require(
        wait_until([&] {
            return button(window, "startSyncButton")->isEnabled() &&
                   text(window, "syncSummary").contains("9 downloaded");
        }) &&
            text(window, "monitorTotals").isEmpty(),
        "one-time synchronization inherited Monitor totals"
    );
}

void test_readable_monitor_results() {
    Fixture fixture;
    const auto lock = app::detail::acquire_runtime_lock(
        fixture.config.state_directory, "monitor"
    );
    monitor::ControlServer server{
        fixture.config.state_directory, fixture.config.sync_data_directory
    };
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file), {}, &localization
    };
    require(
        wait_until([&] {
            return text(window, "monitorStatus").contains("last result: None.");
        }),
        "a Monitor without completed rounds invented a result"
    );
    struct Expected {
        int code;
        const char* english;
        const char* chinese;
    };
    for (const auto& result : {
             Expected{exit_code::success, "Succeeded", "成功"},
             Expected{
                 exit_code::blocked,
                 "Incomplete: blocked items",
                 "未完成：存在阻止项"
             },
             Expected{exit_code::failure, "Failed", "失败"},
             Expected{exit_code::cancelled, "Cancelled", "已取消"},
             Expected{
                 17, "Unknown result (exit code 17)", "未知结果（退出码 17）"
             },
         }) {
        server.finish_round(result.code);
        require(
            wait_until([&] {
                return text(window, "monitorStatus")
                    .contains(QString{"last result: %1."}.arg(result.english));
            }),
            "Monitor result was displayed as an unexplained exit code"
        );
        localization.set_language(gui::Language::simplified_chinese);
        require(
            wait_until([&] {
                return text(window, "monitorStatus")
                    .contains(QString::fromUtf8(result.chinese));
            }),
            "Monitor result did not retranslate into a readable Chinese label"
        );
        localization.set_language(gui::Language::english);
        require(
            wait_until([&] {
                return text(window, "monitorStatus").contains(result.english);
            }),
            "Monitor result did not switch back to English"
        );
    }
}

} // namespace onedrive::test::gui_monitor
