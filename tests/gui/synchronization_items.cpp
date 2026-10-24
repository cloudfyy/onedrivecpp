#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

void test_persistent_blocked_viewer() {
    Fixture fixture;
    const auto directory = account::AccountState::locate(
                               fixture.config.state_directory, fixture.identity
    )
                               .drive_directory;
    {
        storage::ItemDatabase database{directory, fixture.identity};
        database.open();
        storage::BlockedItem item;
        item.drive_id = fixture.identity.drive_id;
        item.remote_id = "notes";
        item.remote_path = "Documents/notes.one";
        item.reason_code = "unsupported_onenote";
        item.reason_message = "OneNote is unsupported";
        database.save_blocked_item(item);
        database.save_blocked_item(item);
        item.remote_id = "upload:local";
        item.remote_path = "local.txt";
        item.reason_code = "upload_resource";
        item.reason_message = "Remote quota exceeded: Ä space 空间不足";
        database.save_blocked_item(item);
    }
    require(
        app::load_blocked_items(fixture.config).size() == 2,
        "offline query did not find the active drive's saved blockers"
    );
    gui::Localization localization{gui::Language::english};
    int run = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config, const events::Observer& observer, std::stop_token) {
            ++run;
            storage::ItemDatabase database{directory, fixture.identity};
            database.open();
            storage::BlockedItem item;
            item.drive_id = fixture.identity.drive_id;
            item.remote_id = "new-" + std::to_string(run);
            item.remote_path = item.remote_id + ".txt";
            item.reason_code = "local_modification";
            item.reason_message = "Local file changed";
            database.save_blocked_item(item);
            observer.blocked_item(
                item.remote_path, item.reason_code, item.reason_message
            );
            if (run == 2) {
                throw std::runtime_error{"independent task failure"};
            }
            return run == 3 ? exit_code::cancelled : exit_code::blocked;
        }
    };
    window.show();
    auto* table = window.findChild<QTreeWidget*>("blockedItemsTable");
    auto* search = window.findChild<QLineEdit*>("blockedItemsFilter");
    auto* reason = window.findChild<QComboBox*>("blockedItemsReason");
    auto* details = window.findChild<QPlainTextEdit*>("blockedItemsDetails");
    require(
        table && search && reason && details &&
            wait_until([&] { return table->topLevelItemCount() == 2; }),
        "startup did not display persisted blockers"
    );
    search->setText("DOCUMENTS");
    require(table->topLevelItemCount() == 1, "blocked path search failed");
    table->setCurrentItem(table->topLevelItem(0));
    require(
        details->toPlainText().contains("Documents/notes.one") &&
            details->toPlainText().contains("Recorded discoveries: 2") &&
            details->toPlainText().contains("This policy cannot be changed."),
        "blocked details lost the reason, path or discovery count"
    );
    search->clear();
    reason->setCurrentIndex(reason->findData("upload_resource"));
    require(
        table->topLevelItemCount() == 1 &&
            table->topLevelItem(0)->text(0) == "local.txt",
        "blocked reason filter failed"
    );
    search->setText("ä SPACE");
    require(
        table->topLevelItemCount() == 1,
        "shared blocker filtering lost GUI Unicode case-insensitive search"
    );
    search->setText("空间不足");
    require(
        table->topLevelItemCount() == 1,
        "shared blocker filtering lost UTF-8 message search"
    );
    search->setText("UNSUPPORTED_ONENOTE");
    require(
        table->topLevelItemCount() == 0,
        "shared blocker filtering did not combine reason and text filters"
    );
    reason->setCurrentIndex(0);
    require(
        table->topLevelItemCount() == 1,
        "shared blocker filtering omitted reason-code search"
    );
    search->clear();
    for (int index = 1; index <= 3; ++index) {
        auto* start = button(
            window, index == 1 ? "previewSyncButton" : "startSyncButton"
        );
        start->click();
        require(
            wait_until([&] {
                return start->isEnabled() &&
                       table->topLevelItemCount() == 2 + index;
            }),
            "preview/failure/cancellation lost persistent discoveries"
        );
    }
    button(window, "clearLogsButton")->click();
    require(
        table->topLevelItemCount() == 5, "clearing logs erased blocked items"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return window.findChild<QGroupBox*>("blockedItemsGroup")->title() ==
                   "阻止项（5）";
        }),
        "blocked viewer was not translated"
    );
    search->setText("notes.one");
    table->setCurrentItem(table->topLevelItem(0));
    require(
        details->toPlainText().contains("此策略不可修改"),
        "fixed OneNote policy was not translated or exposed in details"
    );
    test::write_file(directory / "drive.json", "{ invalid metadata");
    button(window, "refreshBlockedItemsButton")->click();
    require(
        wait_until([&] {
            return window.findChild<QLabel*>("blockedItemsStatus")
                ->text()
                .contains("无法加载阻止项");
        }),
        "database query failure was shown as an empty successful result"
    );
}

void test_blocked_policy_descriptions() {
    struct PolicyCase {
        const char* reason;
        const char* english;
        const char* chinese;
    };
    constexpr std::array cases{
        PolicyCase{
            "unsupported_onenote",
            "Fixed policy: skip unsupported content and protect local copies. "
            "This policy cannot be changed.",
            "固定策略：跳过不支持的内容并保护本地副本。此策略不可修改。",
        },
        PolicyCase{
            "malware_detected",
            "Fixed safety policy: unsafe operations cannot be forced.",
            "固定安全策略：不允许强制执行不安全操作。",
        },
        PolicyCase{
            "local_modification",
            "Uses the configured local-conflict policy; changes are applied "
            "only after validation.",
            "遵循已配置的本地冲突策略，仅在验证通过后应用变更。",
        },
        PolicyCase{
            "upload_delete_limit",
            "Uses the configured remote-deletion safeguard; it is never "
            "bypassed automatically.",
            "遵循已配置的远端删除保护，不会自动绕过。",
        },
        PolicyCase{
            "upload_remote_quota",
            "Retry after resources recover; clear only after verified "
            "resolution.",
            "资源恢复后重试，确认问题已解决后才移除记录。",
        },
        PolicyCase{
            "upload_remote_conflict",
            "Revalidate the conflict before retrying; unsafe operations cannot "
            "be forced.",
            "重新验证冲突后再重试，不允许强制执行不安全操作。",
        },
        PolicyCase{
            "upload_delete_conflict",
            "Revalidate the conflict before retrying; unsafe operations cannot "
            "be forced.",
            "重新验证冲突后再重试，不允许强制执行不安全操作。",
        },
        PolicyCase{
            "future_reason",
            "Unrecognized reason: keep blocked, with no automatic retry or "
            "override.",
            "无法识别的原因：保留阻止，不自动重试，也不允许强制绕过。",
        },
    };
    Fixture fixture;
    const auto directory = account::AccountState::locate(
                               fixture.config.state_directory, fixture.identity
    )
                               .drive_directory;
    {
        storage::ItemDatabase database{directory, fixture.identity};
        database.open();
        for (const auto& policy : cases) {
            storage::BlockedItem item;
            item.drive_id = fixture.identity.drive_id;
            item.remote_id = policy.reason;
            item.remote_path = item.remote_id + ".txt";
            item.reason_code = policy.reason;
            item.reason_message = "Saved diagnostic";
            database.save_blocked_item(item);
        }
    }
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file), {}, &localization
    };
    window.show();
    auto* table = window.findChild<QTreeWidget*>("blockedItemsTable");
    auto* details = window.findChild<QPlainTextEdit*>("blockedItemsDetails");
    require(
        table && details && wait_until([&] {
            return table->topLevelItemCount() == static_cast<int>(cases.size());
        }),
        "policy description fixtures were not loaded"
    );
    for (const auto language :
         {gui::Language::english, gui::Language::simplified_chinese}) {
        localization.set_language(language);
        for (const auto& policy : cases) {
            const auto rows = table->findItems(
                QString::fromUtf8(policy.reason), Qt::MatchExactly, 1
            );
            require(rows.size() == 1, "policy description row is missing");
            table->setCurrentItem(rows.front());
            const auto* expected = language == gui::Language::english
                                       ? policy.english
                                       : policy.chinese;
            require(
                details->toPlainText().contains(QString::fromUtf8(expected)),
                "blocked strategy description was wrong or untranslated"
            );
        }
    }
}

void test_blocked_query_discards_stale_results() {
    Fixture first;
    Fixture second;
    gui::SyncController controller{gui::SynchronizationFunction{}};
    int delivered = 0;
    QString query_error;
    QObject::connect(
        &controller,
        &gui::SyncController::blocked_items_ready,
        [&](const auto& items, const QString& error) {
            ++delivered;
            query_error = error;
            require(
                items.empty(), "stale account blockers leaked into a new query"
            );
        }
    );
    test::write_file(first.file, "invalid = [");
    controller.refresh_blocked_items(first.file);
    controller.refresh_blocked_items(second.file);
    require(
        wait_until([&] { return delivered != 0; }) && delivered == 1 &&
            query_error.isEmpty(),
        "superseded query emitted stale data/errors"
    );
}

void test_controller_file_history() {
    Fixture fixture;
    test::write_file(
        fixture.file,
        test::read_file(fixture.file) + "\n[history]\nmaximum_entries = 3\n"
    );
    std::atomic_bool emitted{false};
    int run = 0;
    gui::SyncController controller{[&](config::Config,
                                       const events::Observer& observer,
                                       std::stop_token) {
        const int first = run++ * 25;
        for (int index = first; index < first + 25; ++index) {
            observer.file_operation({.path = std::to_string(index)});
        }
        for (int index = 0; index < 600; ++index) {
            observer.message(events::MessageKind::debug, "test", "diagnostic");
        }
        emitted = true;
        return exit_code::success;
    }};
    bool finished = false;
    std::vector<events::FileOperationEvent> received;
    std::size_t retained = 0;
    std::size_t omitted = 0;
    const auto ui_thread = std::this_thread::get_id();
    QObject::connect(
        &controller,
        &gui::SyncController::file_operations_ready,
        [&](const auto& operations,
            std::size_t count,
            std::size_t skipped,
            std::size_t limit) {
            require(
                std::this_thread::get_id() == ui_thread &&
                    (limit == 3 || limit == 1),
                "file history was delivered off-thread or lost its limit"
            );
            received.insert(
                received.end(), operations.begin(), operations.end()
            );
            retained = count;
            omitted = skipped;
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int, const QString&, bool) { finished = true; }
    );
    for (int attempt = 0; attempt < 2; ++attempt) {
        emitted = false;
        finished = false;
        received.clear();
        controller.start(fixture.file, false);
        QElapsedTimer timer;
        timer.start();
        while (!emitted && timer.elapsed() < 5000) {
            QThread::msleep(1);
        }
        require(
            emitted && wait_until([&] { return finished; }) && retained == 3 &&
                omitted == static_cast<std::size_t>((attempt + 1) * 25 - 3) &&
                received.size() == 3 &&
                received.front().path == std::to_string(attempt * 25 + 22) &&
                received.back().path == std::to_string(attempt * 25 + 24),
            "log overflow lost file history or a new task reset session "
            "retention"
        );
    }
    controller.set_file_history_limit(1);
    require(
        wait_until([&] { return retained == 1 && omitted == 49; }),
        "shrinking file history did not report evicted records"
    );
    controller.set_file_history_limit(3);
    QCoreApplication::processEvents();
    require(
        retained == 1 && omitted == 49,
        "increasing the limit resurrected discarded history"
    );
}

void test_window_file_history() {
    Fixture fixture;
    test::write_file(
        fixture.file,
        test::read_file(fixture.file) + "\n[history]\nmaximum_entries = 2\n"
    );
    gui::Localization localization{gui::Language::english};
    int runs = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config config,
            const events::Observer& observer,
            std::stop_token) {
            ++runs;
            if (config.dry_run) {
                return exit_code::success;
            }
            if (runs == 1) {
                for (const auto& operation : {
                         events::FileOperationEvent{
                             .action = events::FileAction::downloaded,
                             .path = "first.txt"
                         },
                         events::FileOperationEvent{
                             .action = events::FileAction::uploaded,
                             .path = "Ä-file.txt",
                             .drive_id = "drive",
                             .local_path = "/local/Ä-file.txt"
                         },
                         events::FileOperationEvent{
                             .action = events::FileAction::moved_remotely,
                             .path = "照片/after.txt",
                             .previous_path = "before.txt",
                             .drive_id = "drive"
                         },
                     }) {
                    observer.file_operation(operation);
                }
                return exit_code::success;
            }
            observer.file_operation(
                {.action = events::FileAction::deleted_locally,
                 .path = "deleted.txt",
                 .drive_id = "drive"}
            );
            return exit_code::failure;
        }
    };
    window.show();
    auto* table = window.findChild<QTreeWidget*>("fileHistoryTable");
    auto* filter = window.findChild<QLineEdit*>("fileHistoryFilter");
    auto* status = window.findChild<QLabel*>("fileHistoryStatus");
    require(
        table && filter && status && table->topLevelItemCount() == 0,
        "file history did not start empty"
    );
    auto* start = button(window, "startSyncButton");
    start->click();
    require(
        wait_until([&] {
            return start->isEnabled() && table->topLevelItemCount() == 2;
        }) &&
            table->topLevelItem(0)->text(2) == "Ä-file.txt" &&
            table->topLevelItem(0)->toolTip(2) == "/local/Ä-file.txt" &&
            table->topLevelItem(1)->text(1) == "Moved remotely" &&
            table->topLevelItem(1)->text(3) == "before.txt" &&
            !table->topLevelItem(1)->text(0).isEmpty() &&
            status->text().contains("1 older operations omitted"),
        "file history lost completion time, action, paths or retained-limit "
        "status"
    );
    filter->setText("ä-FILE");
    require(
        !table->topLevelItem(0)->isHidden() &&
            table->topLevelItem(1)->isHidden(),
        "file history search was not Unicode case-insensitive"
    );
    filter->setText("照片");
    require(
        table->topLevelItem(0)->isHidden() &&
            !table->topLevelItem(1)->isHidden(),
        "file history search lost Unicode paths"
    );
    filter->clear();
    button(window, "clearLogsButton")->click();
    require(
        table->topLevelItemCount() == 2, "clearing logs erased file history"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return table->headerItem()->text(1) == "动作" &&
                   table->topLevelItem(1)->text(1) == "远端移动";
        }),
        "file history did not retranslate retained actions"
    );
    localization.set_language(gui::Language::english);
    require(
        wait_until([&] {
            return table->topLevelItem(1)->text(1) == "Moved remotely";
        }),
        "file history could not switch back to English"
    );
    start->click();
    require(
        wait_until([&] {
            return runs == 2 && start->isEnabled() &&
                   table->topLevelItem(1)->text(1) == "Deleted locally";
        }) &&
            table->topLevelItemCount() == 2 &&
            table->topLevelItem(0)->text(2) == "照片/after.txt" &&
            status->text().contains("2 older operations omitted"),
        "a later task or failing round discarded already completed operations"
    );
    auto* preview = button(window, "previewSyncButton");
    preview->click();
    require(
        wait_until([&] { return runs == 3 && preview->isEnabled(); }) &&
            table->topLevelItemCount() == 2 &&
            status->text().contains("2 older operations omitted"),
        "preview reset or invented completed-file history"
    );
}

void test_window_all_file_actions() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    const std::vector<events::FileAction> actions{
        events::FileAction::downloaded,
        events::FileAction::uploaded,
        events::FileAction::moved_locally,
        events::FileAction::moved_remotely,
        events::FileAction::deleted_locally,
        events::FileAction::deleted_remotely,
    };
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config, const events::Observer& observer, std::stop_token) {
            for (std::size_t index = 0; index < actions.size(); ++index) {
                observer.file_operation({
                    .completed_at =
                        std::chrono::system_clock::time_point{
                            std::chrono::milliseconds{123}
                        },
                    .action = actions[index],
                    .path = "文件-" + std::to_string(index),
                    .previous_path = index == 2 || index == 3
                                         ? "before-" + std::to_string(index)
                                         : "",
                    .drive_id = "drive-" + std::to_string(index),
                    .directory = index >= 2,
                    .recovered = index == 3 || index == 5,
                });
            }
            return exit_code::success;
        }
    };
    window.show();
    auto* table = window.findChild<QTreeWidget*>("fileHistoryTable");
    auto* filter = window.findChild<QLineEdit*>("fileHistoryFilter");
    auto* start = button(window, "startSyncButton");
    start->click();
    require(
        wait_until([&] {
            return start->isEnabled() && table->topLevelItemCount() == 6;
        }),
        "not every file action reached the GUI"
    );
    const QStringList english{
        "Downloaded",
        "Uploaded",
        "Moved locally (folder)",
        "Moved remotely (folder) (recovered)",
        "Deleted locally (folder)",
        "Deleted remotely (folder) (recovered)",
    };
    const QStringList chinese{
        "已下载",
        "已上传",
        "本地移动（文件夹）",
        "远端移动（文件夹）（恢复完成）",
        "本地删除（文件夹）",
        "远端删除（文件夹）（恢复完成）",
    };
    const auto labels_match = [&](const QStringList& expected) {
        if (table->topLevelItemCount() != expected.size()) {
            return false;
        }
        for (int index = 0; index < table->topLevelItemCount(); ++index) {
            const auto* row = table->topLevelItem(index);
            if (row->text(1) != expected[index] ||
                row->data(0, Qt::UserRole).toLongLong() != 123 ||
                !row->text(0).endsWith(".123")) {
                return false;
            }
        }
        return true;
    };
    require(
        labels_match(english),
        "file action labels or millisecond timestamps were lost"
    );
    for (const auto& query :
         {QString{"drive-4"}, QString{"  before-2  "}, QString{"recovered"}}) {
        filter->setText(query);
        for (int index = 0; index < table->topLevelItemCount(); ++index) {
            const bool expected = query == "drive-4" ? index == 4
                                  : query.trimmed() == "before-2"
                                      ? index == 2
                                      : index == 3 || index == 5;
            require(
                table->topLevelItem(index)->isHidden() != expected,
                "file history filter missed a drive, previous path or action"
            );
        }
    }
    filter->clear();
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] { return labels_match(chinese); }),
        "folder/recovery markers or actions did not translate into Chinese"
    );
    localization.set_language(gui::Language::english);
    require(
        wait_until([&] { return labels_match(english); }),
        "switching language changed retained action metadata"
    );
}

void test_unsupported_format_reporting() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [](config::Config, const events::Observer& observer, std::stop_token) {
            observer.blocked_item(
                "notes.one", "unsupported_onenote", "OneNote is unsupported"
            );
            observer.blocked_item(
                "future.bin",
                "unsupported_future",
                "Future format is unsupported"
            );
            observer.section(
                "execution_summary",
                "Synchronization summary:",
                {{.label = "unsupported format items:",
                  .key = "unsupported_items",
                  .value = "2"}}
            );
            return exit_code::blocked;
        }
    };
    window.show();
    auto* sync = button(window, "startSyncButton");
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
    auto* level = window.findChild<QComboBox*>("logLevelFilter");
    sync->click();
    require(
        messages && level && wait_until([&] { return sync->isEnabled(); }),
        "unsupported-format task did not finish"
    );
    require(
        messages->toPlainText().contains(
            "Unsupported format: notes.one [unsupported_onenote]"
        ) &&
            messages->toPlainText().contains(
                "Unsupported format: future.bin [unsupported_future]"
            ) &&
            messages->toPlainText().contains("unsupported format items: 2") &&
            window.findChild<QLabel*>("overviewSyncStatus")
                ->text()
                .contains("incomplete"),
        "GUI concealed unsupported files/count or reported complete success"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        wait_until([&] {
            return messages->toPlainText().contains(
                       "不支持的格式：notes.one [unsupported_onenote]"
                   ) &&
                   messages->toPlainText().contains("不支持格式的项目数： 2") &&
                   messages->toPlainText().contains(
                       "已跳过；保留已有本地文件。"
                   );
        }),
        "unsupported-format history was not translated"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::warning))
    );
    require(
        messages->toPlainText().contains("unsupported_onenote") &&
            messages->toPlainText().contains("unsupported_future") &&
            !messages->toPlainText().contains("不支持格式的项目数："),
        "unsupported warnings did not respect the GUI severity threshold"
    );
}

} // namespace onedrive::test::gui_sync
