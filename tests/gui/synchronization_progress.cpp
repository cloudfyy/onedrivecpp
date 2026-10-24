#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

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

void test_progress_color() {
    Fixture fixture;
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    auto* progress = window.findChild<QProgressBar*>("syncProgress");
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    require(progress && tabs, "missing progress color controls");
    tabs->setCurrentWidget(window.findChild<QWidget*>("syncPage"));
    window.show();
    progress->show();
    const QColor blue{"#0078d4"};
    for (const bool dark : {false, true}) {
        auto palette = progress->palette();
        palette.setColor(
            QPalette::Button, dark ? QColor{"#303030"} : QColor{"#f0f0f0"}
        );
        palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
        palette.setColor(QPalette::Highlight, Qt::green);
        progress->setPalette(palette);
        for (const int value : {0, 1, 50, 100}) {
            progress->setValue(value);
            QCoreApplication::processEvents();
            require(
                progress->text() == QString::number(value) + "%" &&
                    progress->alignment() == Qt::AlignCenter,
                "progress text was not a centered numeric percentage"
            );
            const auto image = progress->grab().toImage().convertToFormat(
                QImage::Format_RGB32
            );
            std::size_t blue_pixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixelColor(x, y) == blue) {
                        ++blue_pixels;
                    }
                }
            }
            require(
                !image.isNull(), "progress rendering returned an empty image"
            );
            const auto area = static_cast<std::size_t>(image.width()) *
                              static_cast<std::size_t>(image.height());
            require(
                value == 0   ? blue_pixels == 0
                : value == 1 ? blue_pixels > 0 && blue_pixels < area / 20
                             : blue_pixels > area / 8,
                "progress fill was not blue or misrepresented empty/small "
                "progress"
            );
            if (value == 50) {
                require(
                    image.pixelColor(
                        image.width() * 3 / 4, image.height() / 2
                    ) != blue,
                    "half-complete progress painted the unfilled track blue"
                );
            }
        }
    }
}

void test_upload_progress_phases() {
    Fixture fixture;
    gui::Localization localization{gui::Language::english};
    std::atomic<int> step{};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        &localization,
        [&](config::Config,
            const events::Observer& observer,
            std::stop_token token) {
            observer.sync_context(sync::SyncMode::upload_only, false);
            for (int stage = 0; stage < 4; ++stage) {
                while (step < stage && !token.stop_requested()) {
                    QThread::msleep(1);
                }
                if (token.stop_requested()) {
                    return exit_code::cancelled;
                }
                if (stage == 0) {
                    observer.upload_progress({});
                } else if (stage == 1) {
                    observer.download_progress(
                        1, 1, 10, 10, util::ProgressState::completed
                    );
                    observer.upload_progress(
                        {.completed_files = 0,
                         .file_count = 1,
                         .uploaded = 50,
                         .total = 100,
                         .metrics = {
                             .bytes_per_second = 25,
                             .estimated_seconds_remaining = 2
                         }}
                    );
                } else if (stage == 2) {
                    observer.upload_progress(
                        {.completed_files = 1,
                         .file_count = 1,
                         .uploaded = 100,
                         .total = 100,
                         .state = util::ProgressState::completed}
                    );
                    observer.sync_phase(events::SyncPhase::applying);
                } else {
                    observer.sync_summary(
                        {.downloaded = 1,
                         .uploaded = 1,
                         .moved = 2,
                         .deleted = 3,
                         .blocked = 1}
                    );
                }
            }
            return exit_code::blocked;
        }
    };
    auto* start = button(window, "startSyncButton");
    auto* progress = window.findChild<QProgressBar*>("uploadProgress");
    auto* download_progress = window.findChild<QProgressBar*>("syncProgress");
    auto* status = window.findChild<QLabel*>("syncStatus");
    auto* upload = window.findChild<QLabel*>("uploadStatus");
    auto* hint = window.findChild<QLabel*>("syncProgressHint");
    auto* summary = window.findChild<QLabel*>("syncSummary");
    auto* context = window.findChild<QLabel*>("syncContext");
    require(
        progress && upload && summary && context,
        "missing upload status controls"
    );
    start->click();
    require(
        wait_until([&] {
            return hint->text().contains("Upload totals are unknown");
        }) &&
            progress->isHidden(),
        "unknown upload totals produced a percentage"
    );
    step = 1;
    require(
        wait_until([&] { return progress->value() == 50; }) &&
            !progress->isHidden() && status->text() == "Uploading files..." &&
            upload->text().contains("about 2 s remaining") &&
            context->text().contains("Upload only") &&
            window.findChild<QLabel*>("transferStatus")
                ->text()
                .contains("Downloads:"),
        "upload progress replaced download state or lost direction/rate/ETA"
    );
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(
        status->text() == "正在上传文件..." &&
            progress->accessibleName() == "上传进度" &&
            upload->text().startsWith("上传：") &&
            context->text().contains("仅上传") && progress->value() == 50,
        "upload state did not translate without resetting progress"
    );
    step = 2;
    require(
        wait_until([&] { return status->text() == "正在应用变更..."; }) &&
            progress->value() == 100 && !start->isEnabled() &&
            download_progress->isHidden(),
        "upload 100% was mistaken for overall synchronization completion"
    );
    step = 3;
    require(
        wait_until([&] { return start->isEnabled(); }) &&
            status->text().contains("同步未完成") &&
            summary->text().contains("上传 1") &&
            summary->text().contains("最近结束"),
        "blocked round lost its result, counters, or finish time"
    );
    step = 0;
    start->click();
    require(
        wait_until([&] {
            return !start->isEnabled() && progress->value() == 0;
        }) &&
            progress->isHidden() && !upload->text().contains("100"),
        "new round retained stale upload progress"
    );
    button(window, "stopSyncButton")->click();
    require(
        wait_until([&] { return start->isEnabled(); }),
        "upload progress task did not stop safely"
    );
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
                progress->text() == "0%" &&
                progress->alignment() == Qt::AlignCenter,
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
                progress->value() == 50 && progress->text() == "50%",
            "byte progress or terminal newline handling was incorrect"
        );
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            progress->value() == 50 && progress->text() == "50%" &&
                progress->alignment() == Qt::AlignCenter &&
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
            observer.message(
                events::MessageKind::information,
                "pending_delta_resumed",
                "Restoring saved remote changes; checking only subsequent "
                "changes and rebuilding the local plan..."
            );
            observer.message(
                events::MessageKind::information,
                "pending_delta_invalidated",
                "Saved remote changes no longer match the account, filters, "
                "hash policy, unsupported formats or applied cursor; fetching "
                "remote changes again..."
            );
            observer.message(
                events::MessageKind::information,
                "pending_delta_hierarchy_changed",
                "Remote directory changes or unidentified deletions may "
                "affect saved paths; fetching the full remote state..."
            );
            observer.section(
                "plan",
                "Plan:",
                {{.label = "downloads:", .key = "downloads", .value = "2"}}
            );
            observer.blocked_item(
                "blocked.txt", "conflict", "keep local content"
            );
            spdlog::info("Downloading '{}' ({} bytes)", "example.txt", 100);
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
            messages->toPlainText().contains(
                "Downloading 'example.txt' (100 bytes)"
            ) &&
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
    auto* language_selector = window.findChild<QComboBox*>("languageSelector");
    require(
        language_selector && language_selector->isEnabled() &&
            button(window, "aboutButton")->isEnabled(),
        "header controls were disabled during synchronization"
    );
    language_selector->setCurrentIndex(language_selector->findData(
        static_cast<int>(gui::Language::simplified_chinese)
    ));
    QCoreApplication::processEvents();
    require(
        sync->text() == "立即同步" && stop->text() == "停止" &&
            status->text() == "正在下载文件…" && progress->value() == 50 &&
            messages->toPlainText().contains(
                "远端变更：2 个文件，1 个文件夹，1 项删除。"
            ) &&
            messages->toPlainText().contains("警告：backend detail %1") &&
            messages->toPlainText().contains(
                "正在恢复已保存的远端变更；仅检查后续增量并重新生成本地计划…"
            ) &&
            messages->toPlainText().contains(
                "已保存的远端变更与账号、筛选、哈希策略、不支持格式规则或已应用"
                "游标不匹配；"
                "正在重新获取远端变更…"
            ) &&
            messages->toPlainText().contains(
                "远端目录变更或未识别的删除可能影响已保存的路径；正在获取完整远"
                "端状态…"
            ) &&
            messages->toPlainText().contains("阻塞：blocked.txt [conflict]") &&
            draft->text() == "unsaved draft" && calls == 1 &&
            !sync->isEnabled() && stop->isEnabled(),
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

} // namespace onedrive::test::gui_sync
