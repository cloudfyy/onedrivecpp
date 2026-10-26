#include "localization_support.hpp"
#include "localization_cases.hpp"

namespace onedrive::test::gui_localization {

void test_states_and_messages(bool chinese) {
    require(
        QFile::exists(":/i18n/onedrive_zh_CN.qm") &&
            QFile::exists(":/i18n/qtbase_zh_CN.qm") &&
            QFile::exists(":/icons/onedrive-cpp.svg") &&
            QFile::exists(":/icons/onedrive-cpp-16.png") &&
            QFile::exists(":/icons/onedrive-cpp-24.png") &&
            QFile::exists(":/icons/onedrive-cpp-32.png") &&
            QFile::exists(":/icons/onedrive-cpp-48.png") &&
            QFile::exists(":/icons/onedrive-cpp-64.png") &&
            QFile::exists(":/icons/onedrive-cpp-128.png") &&
            QFile::exists(":/icons/onedrive-cpp-256.png") &&
            QFile::exists(":/icons/onedrive-cpp-512.png") &&
            QFile::exists(":/icons/onedrive-cpp-1024.png"),
        "translations or icons were not embedded in the executable"
    );
    require(
        gui::account_status_text(gui::AccountStatus::no_account) ==
                expected(chinese, "No active account", "无活动账号") &&
            gui::account_status_text(
                gui::AccountStatus::credentials_available
            ) ==
                expected(
                    chinese, "Account credentials available", "账号凭据可用"
                ) &&
            gui::account_status_text(gui::AccountStatus::no_credentials) ==
                expected(
                    chinese,
                    "Active account has no saved credentials",
                    "活动账号没有已保存的凭据"
                ),
        "account state translations were incorrect"
    );
    require(
        gui::sync_mode_text(sync::SyncMode::bidirectional) ==
                expected(chinese, "Bidirectional", "双向同步") &&
            gui::sync_mode_text(sync::SyncMode::upload_only) ==
                expected(chinese, "Upload only", "仅上传") &&
            gui::sync_mode_text(sync::SyncMode::download_only) ==
                expected(chinese, "Download only", "仅下载") &&
            gui::delete_policy_text(sync::DeletePolicy::propagate) ==
                expected(chinese, "Propagate deletions", "传播删除操作") &&
            gui::delete_policy_text(sync::DeletePolicy::preserve) ==
                expected(chinese, "Preserve destination files", "保留目标文件"),
        "configuration enum translations were incorrect"
    );
    const auto detail = QString::fromUtf8("server detail: /配置/%2");
    require(
        gui::MainWindow::tr("Login failed: %1").arg(detail) ==
                expected(chinese, "Login failed: ", "登录失败：") + detail &&
            QCoreApplication::translate(
                "Application", "Cannot start OneDrive client:\n%1"
            )
                    .arg(detail) == expected(
                                        chinese,
                                        "Cannot start OneDrive client:\n",
                                        "无法启动 OneDrive 客户端：\n"
                                    ) + detail,
        "error translation did not preserve backend detail"
    );
    require(
        gui::MainWindow::tr(
            "\n\nSync directory:\n%1\n→ %2\n"
            "Inspect a dry run before the next synchronization."
        )
                .arg("/old/%2-目录", "/new/%1-目录") ==
            expected(
                chinese,
                "\n\nSync directory:\n/old/%2-目录\n→ /new/%1-目录\n"
                "Inspect a dry run before the next synchronization.",
                "\n\n同步目录：\n/old/%2-目录\n→ /new/%1-目录\n"
                "下次同步前请检查试运行结果。"
            ),
        "directory confirmation placeholders were incorrect"
    );
    require(
        gui::MainWindow::tr(
            "Import this configuration to the active user configuration?\n\n"
            "Sync directory: %1\nState directory: %2\nSync mode: %3\n"
            "Deletion policy: %4\n\n"
            "Relative file references will be resolved from the active "
            "configuration directory. The current configuration will be "
            "backed up."
        )
                .arg(
                    "/sync/中文",
                    "/state/中文",
                    gui::sync_mode_text(sync::SyncMode::upload_only),
                    gui::delete_policy_text(sync::DeletePolicy::preserve)
                ) ==
            expected(
                chinese,
                "Import this configuration to the active user "
                "configuration?\n\n"
                "Sync directory: /sync/中文\nState directory: /state/中文\n"
                "Sync mode: Upload only\nDeletion policy: Preserve destination "
                "files\n\n"
                "Relative file references will be resolved from the active "
                "configuration directory. The current configuration will be "
                "backed up.",
                "将此配置导入到当前用户配置吗？\n\n"
                "同步目录：/sync/中文\n状态目录：/state/中文\n"
                "同步模式：仅上传\n删除策略：保留目标文件\n\n"
                "相对文件引用将基于当前配置目录解析。当前配置将被备份。"
            ),
        "import confirmation placeholders were incorrect"
    );
    require(
        gui::MainWindow::tr("TOML files (*.toml);;All files (*)") ==
            expected(
                chinese,
                "TOML files (*.toml);;All files (*)",
                "TOML 文件 (*.toml);;所有文件 (*)"
            ),
        "file dialog filters were not translated"
    );
}

void test_window_and_settings(bool chinese) {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    const auto state = gui::load_app_state_view_model(file);
    gui::MainWindow window{state};
    require(
        window.windowTitle() ==
            expected(chinese, "onedrive client", "OneDrive 客户端"),
        "window title was not localized or changed the English title"
    );
    const auto* tabs = window.findChild<QTabWidget*>();
    require(
        tabs && tabs->tabText(0) == expected(chinese, "Overview", "概览") &&
            tabs->count() == 5 &&
            tabs->tabText(1) == expected(chinese, "Sync", "同步") &&
            tabs->tabText(2) == expected(chinese, "Files", "文件") &&
            tabs->tabText(3) == expected(chinese, "Settings", "设置") &&
            tabs->tabText(4) == expected(chinese, "Logs", "日志"),
        "tab labels were not translated"
    );
    for (const auto& pair : {
             std::pair{"Account", "账号"},
             std::pair{"Configuration file", "配置文件"},
             std::pair{"Sync directory", "同步目录"},
             std::pair{"State directory", "状态目录"},
             std::pair{"Login", "登录"},
             std::pair{"Browser login", "浏览器登录"},
             std::pair{"Appearance", "外观"},
             std::pair{
                 "Follow system light/dark appearance", "跟随系统浅色／深色外观"
             },
         }) {
        require(
            has_label(window, expected(chinese, pair.first, pair.second)),
            "a form label was not translated"
        );
    }
    require(
        has_label(window, QString::fromStdString(file.string())) &&
            has_label(
                window, gui::account_status_text(gui::AccountStatus::no_account)
            ),
        "window changed a path or did not render the translated account state"
    );
    for (const auto& pair : {
             std::pair{"Sign in", "登录账号"},
             std::pair{"Log out", "退出登录"},
             std::pair{"Browse…", "浏览…"},
             std::pair{"Import TOML…", "导入 TOML…"},
         }) {
        static_cast<void>(
            button(window, expected(chinese, pair.first, pair.second))
        );
    }
    auto* save = button(window, expected(chinese, "Save settings", "保存设置"));
    bool dialog_seen = false;
    bool dialog_correct = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_seen = dialog != nullptr;
        if (dialog) {
            dialog_correct =
                dialog->windowTitle() ==
                    expected(chinese, "Settings saved", "设置已保存") &&
                dialog->text().startsWith(expected(
                    chinese,
                    "Settings saved. Changes take effect on the next "
                    "operation.\nPrevious configuration backed up "
                    "to:\n",
                    "设置已保存。更改将在下次操作时生效。\n原配置已备份到：\n"
                )) &&
                dialog->text().contains(
                    QString::fromStdString(file.string()) + ".backup."
                ) &&
                (chinese
                     ? dialog->button(QMessageBox::Ok)->text().contains("确定")
                     : dialog->button(QMessageBox::Ok)->text().contains("OK"));
            dialog->accept();
        }
    });
    save->click();
    require(
        dialog_seen && dialog_correct,
        "saved settings dialog or Qt buttons were not localized"
    );
    auto* sync_field = window.findChild<QLineEdit*>("sync.data_directory");
    const auto previous = test::read_file(file);
    sync_field->setText("/changed/同步");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_correct =
            dialog &&
            dialog->windowTitle() ==
                expected(
                    chinese, "Confirm directory changes", "确认目录更改"
                ) &&
            dialog->text().contains(
                QString::fromStdString(state.sync_directory.string())
            ) &&
            dialog->text().contains("/changed/同步") &&
            (!chinese ||
             (dialog->button(QMessageBox::Save)->text().contains("保存") &&
              dialog->button(QMessageBox::Cancel)->text().contains("取消")));
        if (dialog) {
            dialog->button(QMessageBox::Cancel)->click();
        }
    });
    save->click();
    require(
        dialog_correct && test::read_file(file) == previous,
        "directory confirmation was not localized or cancel saved changes"
    );
    sync_field->setText(QString::fromStdString(state.sync_directory.string()));
    const auto imported_file = fixture.path() / "import.toml";
    test::write_file(
        imported_file,
        "config_version = 2\n[sync]\ndata_directory = \"/import/%3-同步\"\n"
        "mode = \"upload_only\"\ndelete_policy = \"preserve\"\n"
        "[state]\ndirectory = \"/import/%4-状态\"\n"
    );
    for (const bool confirm : {false, true}) {
        int stage = 0;
        bool import_correct = true;
        QTimer dialogs;
        QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
            auto* modal = QApplication::activeModalWidget();
            if (stage == 0) {
                auto* dialog = qobject_cast<QFileDialog*>(modal);
                if (!dialog) {
                    return;
                }
                import_correct =
                    dialog->windowTitle() ==
                        expected(chinese, "Import configuration", "导入配置") &&
                    dialog->nameFilters().first() ==
                        expected(
                            chinese, "TOML files (*.toml)", "TOML 文件 (*.toml)"
                        );
                dialog->selectFile(
                    QString::fromStdString(imported_file.string())
                );
                ++stage;
                QMetaObject::invokeMethod(
                    dialog, "accept", Qt::QueuedConnection
                );
                return;
            }
            auto* dialog = qobject_cast<QMessageBox*>(modal);
            if (!dialog) {
                return;
            }
            if (stage == 1) {
                import_correct =
                    import_correct &&
                    dialog->windowTitle() ==
                        expected(chinese, "Import configuration", "导入配置") &&
                    dialog->text().contains("/import/%3-同步") &&
                    dialog->text().contains("/import/%4-状态") &&
                    dialog->text().contains(
                        expected(chinese, "Upload only", "仅上传")
                    ) &&
                    dialog->text().contains(expected(
                        chinese, "Preserve destination files", "保留目标文件"
                    ));
                ++stage;
                dialog->button(confirm ? QMessageBox::Yes : QMessageBox::Cancel)
                    ->click();
            } else if (stage == 2 && confirm) {
                import_correct =
                    import_correct &&
                    dialog->windowTitle() ==
                        expected(
                            chinese, "Configuration imported", "配置已导入"
                        ) &&
                    dialog->text().startsWith(expected(
                        chinese, "Configuration imported.", "配置已导入。"
                    )) &&
                    dialog->text().contains(
                        QString::fromStdString(file.string()) + ".backup."
                    );
                ++stage;
                dialog->accept();
            }
        });
        dialogs.start(1);
        button(window, expected(chinese, "Import TOML…", "导入 TOML…"))
            ->click();
        dialogs.stop();
        require(
            import_correct && stage == (confirm ? 3 : 2) &&
                test::read_file(file) ==
                    (confirm ? test::read_file(imported_file) : previous),
            "import dialogs lost translations or placeholders, or cancellation "
            "changed configuration"
        );
    }
    test::write_file(file, "[invalid TOML");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_correct =
            dialog &&
            dialog->windowTitle() ==
                expected(chinese, "Cannot save settings", "无法保存设置") &&
            dialog->text().startsWith(expected(
                chinese, "Settings could not be saved:\n", "设置保存失败：\n"
            )) &&
            dialog->text().size() > 40;
        if (dialog) {
            dialog->accept();
        }
    });
    save->click();
    require(
        dialog_correct,
        "settings error did not retain translated context and backend detail"
    );
}

void test_about_button() {
    test::TemporaryDirectory fixture;
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(write_config(fixture.path())),
        {},
        &localization
    };
    const auto app_icon = gui::application_icon();
    const auto icon = window.windowIcon();
    for (const int extent : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
        require(
            app_icon.availableSizes().contains(QSize{extent, extent}) &&
                icon.availableSizes().contains(QSize{extent, extent}),
            "MainWindow or application_icon() omitted an embedded icon size"
        );
    }
    require(
        !app_icon.isNull() && !icon.isNull() &&
            icon.pixmap(QSize{1024, 1024}).size() == QSize{1024, 1024},
        "MainWindow did not load its multi-resolution and HD application icon"
    );
    for (const bool chinese : {false, true, false}) {
        localization.set_language(
            chinese ? gui::Language::simplified_chinese : gui::Language::english
        );
        QCoreApplication::processEvents();
        auto* about = window.findChild<QPushButton*>("aboutButton");
        require(
            about && about->text() == expected(chinese, "About", "关于") &&
                window.windowTitle() ==
                    expected(chinese, "onedrive client", "OneDrive 客户端"),
            "about button did not translate or altered the window title"
        );
        bool valid = false;
        QTimer::singleShot(0, [&] {
            auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (dialog) {
                valid = !dialog->windowIcon().isNull() &&
                        !dialog->iconPixmap().isNull() &&
                        dialog->windowTitle() == expected(
                                                     chinese,
                                                     "About OneDrive client",
                                                     "关于 OneDrive 客户端"
                                                 ) &&
                        dialog->text().contains(
                            QString::fromLatin1(build_info::version)
                        ) &&
                        dialog->text().contains(
                            "https://github.com/cloudfyy/onedrivecpp"
                        ) &&
                        dialog->text().contains("GPL-3.0-or-later") &&
                        dialog->text().contains(
                            expected(chinese, "Version:", "版本：")
                        );
                dialog->accept();
            }
        });
        about->click();
        require(valid, "about dialog omitted translated project information");
    }
}

} // namespace onedrive::test::gui_localization
