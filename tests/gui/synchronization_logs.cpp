#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

void test_controller_logging() {
    Fixture fixture;
    const auto log_file = fixture.config.state_directory / "sync.log";
    test::write_file(
        fixture.file,
        test::read_file(fixture.file) +
            "\n[logging]\nlevel = \"debug\"\nfile = \"" + log_file.string() +
            "\"\n"
    );
    const auto previous_logger = spdlog::default_logger();
    const auto ui_thread = std::this_thread::get_id();
    gui::SyncController controller{
        [](config::Config, const events::Observer&, std::stop_token) {
            std::jthread download_worker{[] {
                spdlog::trace("hidden trace detail");
                spdlog::debug("download debug detail");
                spdlog::info("Downloading '{}' ({} bytes)", "example.txt", 4);
                spdlog::warn("download warning");
                spdlog::error("download error");
                spdlog::critical("download critical");
            }};
            download_worker.join();
            return exit_code::success;
        }
    };
    std::vector<events::MessageEvent> messages;
    bool finished = false;
    QObject::connect(
        &controller,
        &gui::SyncController::events_ready,
        [&](const auto& batch, std::size_t omitted) {
            require(
                omitted == 0 && std::this_thread::get_id() == ui_thread,
                "backend logs were dropped or delivered off the UI thread"
            );
            for (const auto& event : batch) {
                if (const auto* message =
                        std::get_if<events::MessageEvent>(&event)) {
                    messages.push_back(*message);
                }
            }
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool) {
            require(
                result == exit_code::success && error.isEmpty(),
                ("logging changed the synchronization result: " +
                 error.toStdString())
                    .c_str()
            );
            finished = true;
        }
    );
    controller.start(fixture.file, false);
    require(
        wait_until([&] { return finished; }) && messages.size() == 5 &&
            messages[0].kind == events::MessageKind::debug &&
            messages[0].text == "download debug detail" &&
            messages[1].kind == events::MessageKind::information &&
            messages[1].text == "Downloading 'example.txt' (4 bytes)" &&
            messages[2].kind == events::MessageKind::warning &&
            messages[3].kind == events::MessageKind::error &&
            messages[4].kind == events::MessageKind::critical &&
            spdlog::default_logger() == previous_logger,
        "backend logging lost messages, severity, filtering or logger "
        "restoration"
    );
    for (const auto& message : messages) {
        require(
            message.event == "log" &&
                test::read_file(log_file).find(message.text) !=
                    std::string::npos,
            "GUI log forwarding changed the event name or file logging"
        );
    }
    require(
        test::read_file(log_file).find("hidden trace detail") ==
            std::string::npos,
        "GUI log forwarding ignored the configured logging level"
    );
    const auto source = test::read_file(fixture.file);
    auto trace_source = source;
    trace_source.replace(trace_source.find("\"debug\""), 7, "\"trace\"");
    test::write_file(fixture.file, trace_source);
    messages.clear();
    finished = false;
    controller.start(fixture.file, false);
    require(
        wait_until([&] { return finished; }) && messages.size() == 6 &&
            messages[0].kind == events::MessageKind::trace &&
            messages[0].text == "hidden trace detail" &&
            messages[1].kind == events::MessageKind::debug &&
            messages[2].kind == events::MessageKind::information &&
            messages[3].kind == events::MessageKind::warning &&
            messages[4].kind == events::MessageKind::error &&
            messages[5].kind == events::MessageKind::critical &&
            spdlog::default_logger() == previous_logger,
        "GUI log forwarding did not preserve all six backend levels"
    );
    const std::vector expected_levels{
        events::MessageKind::trace,
        events::MessageKind::debug,
        events::MessageKind::information,
        events::MessageKind::warning,
        events::MessageKind::error,
        events::MessageKind::critical
    };
    for (const auto& [threshold, first] :
         {std::pair{"info", 2U},
          std::pair{"warn", 3U},
          std::pair{"error", 4U},
          std::pair{"critical", 5U},
          std::pair{"off", 6U}}) {
        auto configuration = source;
        configuration.replace(
            configuration.find("\"debug\""),
            7,
            "\"" + std::string{threshold} + "\""
        );
        test::write_file(fixture.file, configuration);
        const auto previous_file = test::read_file(log_file);
        messages.clear();
        finished = false;
        controller.start(fixture.file, false);
        require(
            wait_until([&] { return finished; }) &&
                messages.size() == expected_levels.size() - first &&
                spdlog::default_logger() == previous_logger,
            "GUI log forwarding ignored a configured threshold or off setting"
        );
        for (std::size_t index = 0; index < messages.size(); ++index) {
            require(
                messages[index].kind == expected_levels[first + index],
                "GUI log threshold changed the original backend severity"
            );
        }
        const auto appended =
            test::read_file(log_file).substr(previous_file.size());
        for (const auto& message : messages) {
            require(
                appended.contains(message.text),
                "GUI and file sinks disagree on the configured threshold"
            );
        }
        require(
            first != expected_levels.size() || appended.empty(),
            "logging off still wrote backend messages to the file sink"
        );
    }
}

void test_controller_summaries_survive_log_overflow() {
    Fixture fixture;
    std::atomic_bool emitted{false};
    gui::SyncController controller{
        [&](config::Config, const events::Observer& observer, std::stop_token) {
            for (const auto& summary : {
                     events::SyncSummaryEvent{.downloaded = 1, .blocked = 47},
                     events::SyncSummaryEvent{.blocked = 47},
                     events::SyncSummaryEvent{
                         .downloaded = 2,
                         .uploaded = 3,
                         .moved = 4,
                         .deleted = 5,
                         .blocked = 3
                     },
                 }) {
                observer.sync_summary(summary);
                for (int index = 0; index < 600; ++index) {
                    observer.message(
                        events::MessageKind::debug, "test", "diagnostic"
                    );
                }
            }
            emitted = true;
            return exit_code::success;
        }
    };
    bool finished = false;
    std::size_t omitted = 0;
    std::optional<events::SyncSummaryEvent> last;
    events::SyncSummaryEvent total;
    const auto ui_thread = std::this_thread::get_id();
    QObject::connect(
        &controller,
        &gui::SyncController::summary_ready,
        [&](const auto& summary, const auto& totals) {
            require(
                std::this_thread::get_id() == ui_thread,
                "round summaries were delivered off the UI thread"
            );
            last = summary;
            total = totals;
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::events_ready,
        [&](const auto&, std::size_t dropped) { omitted += dropped; }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int, const QString&, bool) { finished = true; }
    );
    for (int run = 0; run < 2; ++run) {
        emitted = false;
        finished = false;
        omitted = 0;
        last.reset();
        controller.start(fixture.file, false);
        QElapsedTimer elapsed;
        elapsed.start();
        while (!emitted && elapsed.elapsed() < 5000) {
            QThread::msleep(1);
        }
        require(
            emitted && wait_until([&] { return finished; }) && omitted > 0 &&
                last && last->downloaded == 2 && last->blocked == 3 &&
                total.downloaded == 3 && total.uploaded == 3 &&
                total.moved == 4 && total.deleted == 5 && total.blocked == 3,
            "diagnostic overflow lost a summary, summed blockers, or leaked "
            "totals between tasks"
        );
    }
}

void test_controller_logging_after_failure() {
    Fixture fixture;
    const auto previous_logger = spdlog::default_logger();
    int calls = 0;
    gui::SyncController controller{
        [&](config::Config, const events::Observer&, std::stop_token) {
            spdlog::warn("worker diagnostic {}", ++calls);
            if (calls == 1) {
                throw std::runtime_error{"worker failure after logging"};
            }
            return exit_code::success;
        }
    };
    std::vector<events::MessageEvent> messages;
    bool finished = false;
    QObject::connect(
        &controller,
        &gui::SyncController::events_ready,
        [&](const auto& batch, std::size_t omitted) {
            require(omitted == 0, "failure lost a backend diagnostic");
            for (const auto& event : batch) {
                if (const auto* message =
                        std::get_if<events::MessageEvent>(&event)) {
                    messages.push_back(*message);
                }
            }
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool) {
            require(
                messages.size() == 1 &&
                    messages.front().kind == events::MessageKind::warning &&
                    messages.front().text ==
                        "worker diagnostic " + std::to_string(calls) &&
                    messages.front().event == "log" &&
                    spdlog::default_logger() == previous_logger &&
                    !controller.running(),
                "failure or restart lost logs, delivery order or logger cleanup"
            );
            require(
                calls == 1 ? result == exit_code::failure &&
                                 error == "worker failure after logging"
                           : result == exit_code::success && error.isEmpty(),
                "forwarding logs changed failure or restart results"
            );
            finished = true;
        }
    );
    for (int attempt = 1; attempt <= 2; ++attempt) {
        messages.clear();
        finished = false;
        controller.start(fixture.file, false);
        require(
            wait_until([&] { return finished; }) && calls == attempt,
            "failed logging task could not be restarted"
        );
    }
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

} // namespace onedrive::test::gui_sync
