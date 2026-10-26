#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

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
                events::MessageKind::information,
                "completed",
                "successful event %1"
            );
            observer.message(events::MessageKind::trace, "trace", "raw trace");
            observer.message(events::MessageKind::debug, "debug", "raw debug");
            observer.message(
                events::MessageKind::critical, "critical", "raw critical"
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
        tabs && tabs->count() == 5 && tabs->tabText(0) == "Overview" &&
            tabs->tabText(1) == "Sync" && tabs->tabText(2) == "Files" &&
            tabs->tabText(3) == "Settings" && tabs->tabText(4) == "Logs" &&
            tabs->currentWidget() == overview &&
            sync_page->isAncestorOf(sync) && !overview->isAncestorOf(sync) &&
            logs_page->isAncestorOf(messages) && messages->isReadOnly() &&
            scroll_option->isChecked() && level->count() == 7 &&
            level->itemText(1) == "Trace" && level->itemText(2) == "Debug" &&
            level->itemText(6) == "Critical",
        "GUI tasks and logs were not separated into tabs"
    );
    auto* draft = window.findChild<QLineEdit*>("sync.data_directory");
    draft->setText("keep draft");
    require(
        !window.findChild<QPushButton*>("openSyncButton") &&
            !window.findChild<QPushButton*>("viewLogsButton"),
        "GUI retained unnecessary synchronization or log navigation buttons"
    );
    tabs->setCurrentWidget(sync_page);
    sync->click();
    auto* settings_page = window.findChild<QWidget*>("settingsPage");
    tabs->setCurrentWidget(settings_page);
    require(
        wait_until([&] { return sync->isEnabled(); }) &&
            tabs->currentWidget() == settings_page &&
            draft->text() == "keep draft" &&
            window.findChild<QLabel*>("overviewSyncStatus")->text() ==
                "Synchronization completed.",
        "tab switching reset the task, draft or overview summary"
    );
    tabs->setCurrentWidget(logs_page);
    QCoreApplication::processEvents();
    const auto first_history = messages->toPlainText();
    const QRegularExpression timestamp{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\])"
    };
    require(
        tabs->currentWidget() == logs_page &&
            timestamp.match(first_history).hasMatch() &&
            first_history.contains("[Sync] Info: run 1 info 0") &&
            !first_history.contains("saved-token") &&
            first_history.contains(
                "[Sync] Error: raw failure\n/path/file %1"
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
            warnings.contains("Error: raw failure") &&
            warnings.contains("Critical: raw critical") &&
            !warnings.contains("info 0") &&
            !warnings.contains("successful event"),
        "warning threshold lost higher levels or retained lower levels"
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
        tabs->currentWidget() == logs_page && tabs->tabText(4) == "日志" &&
            level->currentData().toInt() ==
                static_cast<int>(events::MessageKind::warning) &&
            level->currentText() == "警告" && copy->text() == "复制日志" &&
            level->toolTip() == "显示此级别及更高级别的日志。" &&
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
            messages->toPlainText().contains("严重：raw critical") &&
            !messages->toPlainText().contains("raw warning"),
        "error filter was incorrect"
    );
    level->setCurrentIndex(
        level->findData(static_cast<int>(events::MessageKind::information))
    );
    require(
        messages->toPlainText().contains("run 1 info 0") &&
            messages->toPlainText().contains("raw warning") &&
            messages->toPlainText().contains("信息：successful event %1") &&
            messages->toPlainText().contains("预览完成。未修改同步文件。") &&
            messages->toPlainText().contains("raw failure") &&
            !messages->toPlainText().contains("raw trace") &&
            !messages->toPlainText().contains("raw debug") &&
            messages->toPlainText().contains("raw critical"),
        "information filter was incorrect"
    );
    const std::vector level_cases{
        std::tuple{events::MessageKind::trace, "Trace", "跟踪", "raw trace"},
        std::tuple{events::MessageKind::debug, "Debug", "调试", "raw debug"},
        std::tuple{
            events::MessageKind::information,
            "Information",
            "信息",
            "successful event %1"
        },
        std::tuple{
            events::MessageKind::warning, "Warning", "警告", "raw warning"
        },
        std::tuple{events::MessageKind::error, "Error", "错误", "raw failure"},
        std::tuple{
            events::MessageKind::critical, "Critical", "严重", "raw critical"
        },
    };
    for (const auto language :
         {gui::Language::simplified_chinese, gui::Language::english}) {
        localization.set_language(language);
        QCoreApplication::processEvents();
        for (const auto& [kind, english, chinese, text] : level_cases) {
            level->setCurrentIndex(level->findData(static_cast<int>(kind)));
            require(
                level->currentText() ==
                    QString::fromUtf8(
                        language == gui::Language::english ? english : chinese
                    ),
                "log threshold label did not translate"
            );
            const auto displayed = messages->toPlainText();
            for (const auto& [other, other_english, other_chinese, marker] :
                 level_cases) {
                require(
                    displayed.contains(QString::fromUtf8(marker)) ==
                        (other >= kind),
                    "log threshold did not include exactly its level and above"
                );
            }
            copy->click();
            require(
                QApplication::clipboard()->text() == displayed,
                "copy ignored the minimum-severity filter"
            );
        }
        level->setCurrentIndex(0);
        for (const auto& [kind, english, chinese, marker] : level_cases) {
            require(
                messages->toPlainText().contains(QString::fromUtf8(marker)),
                "all-levels selection did not restore the complete history"
            );
        }
    }
    require(
        level->toolTip() == "Show messages at this level or higher.",
        "log threshold hint did not translate to English"
    );
    const int before_reload = messages->document()->blockCount();
    button(window, "reloadSettingsButton")->click();
    require(
        messages->toPlainText().contains(
            "[Configuration] Info: Configuration reloaded."
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

void test_live_log_scroll() {
    Fixture fixture;
    std::atomic<int> requested{};
    std::atomic<int> emitted{-1};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config,
            const events::Observer& observer,
            std::stop_token token) {
            for (int phase = 0; phase < 4; ++phase) {
                while (requested < phase && !token.stop_requested()) {
                    QThread::msleep(1);
                }
                if (token.stop_requested()) {
                    return exit_code::cancelled;
                }
                const int count = phase == 2 ? 400 : 120;
                for (int index = 0; index < count; ++index) {
                    observer.message(
                        events::MessageKind::information,
                        "scroll",
                        std::format(
                            "scroll phase {} entry {}\n{}",
                            phase,
                            index,
                            std::string(240, 'x')
                        )
                    );
                }
                emitted = phase;
            }
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        }
    };
    window.show();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
    auto* option = window.findChild<QCheckBox*>("logAutoScroll");
    auto* sync = button(window, "startSyncButton");
    tabs->setCurrentWidget(window.findChild<QWidget*>("logsPage"));
    const auto advance = [&](int phase, int last) {
        requested = phase;
        require(
            wait_until([&] {
                return emitted == phase &&
                       messages->toPlainText().contains(
                           QString{"scroll phase %1 entry %2"}.arg(phase).arg(
                               last
                           )
                       );
            }),
            "scrolling task did not deliver its next log batch"
        );
        QCoreApplication::processEvents();
    };
    const auto visible_text = [&] {
        const auto cursor = messages->cursorForPosition(QPoint{0, 0});
        const auto visible = cursor.block();
        auto header = visible;
        while (header.isValid() && !header.text().startsWith('[')) {
            header = header.previous();
        }
        return std::tuple{
            header.text(),
            visible.blockNumber() - header.blockNumber(),
            cursor.positionInBlock()
        };
    };
    sync->click();
    advance(0, 119);
    auto* scroll = messages->verticalScrollBar();
    require(
        scroll->maximum() > 0 && scroll->value() == scroll->maximum(),
        "live auto-scroll did not follow the latest logs"
    );
    option->setChecked(false);
    const auto at_bottom = visible_text();
    advance(1, 119);
    require(
        visible_text() == at_bottom && scroll->value() < scroll->maximum(),
        "disabling auto-scroll at the bottom still followed live arrivals"
    );
    const auto anchor = messages->document()->find("scroll phase 1 entry 80");
    require(!anchor.isNull(), "cannot locate a retained scroll anchor");
    messages->setTextCursor(anchor);
    messages->centerCursor();
    QCoreApplication::processEvents();
    const auto before_overflow = visible_text();
    advance(2, 399);
    require(
        messages->toPlainText().startsWith("Earlier events omitted:") &&
            visible_text() == before_overflow,
        "bounded history moved the reading position with auto-scroll disabled"
    );
    scroll->setValue(0);
    advance(3, 119);
    require(
        scroll->value() == 0,
        "expired scroll anchors did not stay at the oldest retained logs"
    );
    option->setChecked(true);
    require(
        scroll->value() == scroll->maximum(),
        "re-enabling auto-scroll did not return to the latest logs"
    );
    button(window, "stopSyncButton")->click();
    require(
        wait_until([&] { return sync->isEnabled(); }),
        "scrolling task did not stop"
    );
}

void test_live_log_threshold() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic<int> requested{};
    std::atomic<int> emitted{-1};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config, const events::Observer&, std::stop_token token) {
            for (int phase = 0; phase <= 5; ++phase) {
                while (requested < phase && !token.stop_requested()) {
                    QThread::msleep(1);
                }
                if (token.stop_requested()) {
                    return exit_code::cancelled;
                }
                switch (phase) {
                case 0:
                    spdlog::info("active info");
                    break;
                case 1:
                    spdlog::warn("active warning");
                    break;
                case 2:
                    spdlog::error("active error");
                    break;
                case 3:
                    spdlog::critical("active critical");
                    break;
                case 4:
                    spdlog::info("after clear info");
                    break;
                case 5:
                    spdlog::error("after clear error");
                    break;
                }
                emitted = phase;
            }
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        }
    };
    auto* level = window.findChild<QComboBox*>("logLevelFilter");
    auto* messages = window.findChild<QPlainTextEdit*>("logMessages");
    auto* copy = button(window, "copyLogsButton");
    auto* clear = button(window, "clearLogsButton");
    auto* sync = button(window, "startSyncButton");
    const auto select = [&](events::MessageKind kind) {
        level->setCurrentIndex(level->findData(static_cast<int>(kind)));
    };
    const auto advance = [&](int phase) {
        requested = phase;
        require(
            wait_until([&] { return emitted == phase; }),
            "live logging task did not emit its next phase"
        );
        QCoreApplication::processEvents();
    };
    select(events::MessageKind::error);
    sync->click();
    advance(0);
    advance(1);
    require(
        messages->toPlainText().isEmpty() && !copy->isEnabled() &&
            clear->isEnabled() && !sync->isEnabled(),
        "empty threshold results exposed lower levels or lost hidden history"
    );
    advance(2);
    require(
        messages->toPlainText().contains("Error: active error") &&
            !messages->toPlainText().contains("active warning") &&
            copy->isEnabled(),
        "an arriving error did not satisfy the live error threshold"
    );
    select(events::MessageKind::warning);
    advance(3);
    require(
        messages->toPlainText().contains("Warning: active warning") &&
            messages->toPlainText().contains("Error: active error") &&
            messages->toPlainText().contains("Critical: active critical") &&
            !messages->toPlainText().contains("active info"),
        "changing a live threshold lost history or excluded higher levels"
    );
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(
        level->currentData().toInt() ==
                static_cast<int>(events::MessageKind::warning) &&
            messages->toPlainText().contains("警告：active warning") &&
            messages->toPlainText().contains("错误：active error") &&
            messages->toPlainText().contains("严重：active critical"),
        "language switching changed a running task's minimum severity"
    );
    copy->click();
    require(
        QApplication::clipboard()->text() == messages->toPlainText(),
        "copy lost the higher-severity live messages"
    );
    clear->click();
    advance(4);
    require(
        messages->toPlainText().isEmpty() && !copy->isEnabled() &&
            clear->isEnabled() &&
            level->currentData().toInt() ==
                static_cast<int>(events::MessageKind::warning),
        "clearing live logs reset the threshold or exposed lower-severity "
        "arrivals"
    );
    select(events::MessageKind::information);
    require(
        messages->toPlainText().contains("信息：after clear info") &&
            !messages->toPlainText().contains("active "),
        "changing threshold lost retained arrivals or restored cleared history"
    );
    select(events::MessageKind::warning);
    advance(5);
    require(
        messages->toPlainText().contains("错误：after clear error") &&
            !messages->toPlainText().contains("after clear info") &&
            !messages->toPlainText().contains("active "),
        "a new higher-severity arrival bypassed the live threshold or clear"
    );
    button(window, "stopSyncButton")->click();
    require(
        wait_until([&] { return sync->isEnabled(); }) &&
            messages->toPlainText().contains("错误：after clear error") &&
            !messages->toPlainText().contains("同步已取消"),
        "cancellation lost retained diagnostics or bypassed the minimum "
        "severity"
    );
}

} // namespace onedrive::test::gui_sync
