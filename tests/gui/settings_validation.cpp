#include "settings_support.hpp"
#include "settings_cases.hpp"

namespace onedrive::test::gui_settings {

void test_persistence_rejects_without_writes() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const std::string original =
        "config_version = 2\n[sync]\ndata_directory = 'sync'\n"
        "[state]\ndirectory = 'state'\n";
    test::write_file(file, original);
    for (const auto& patches :
         {std::vector<config::SettingPatch>{
              {"sync", "data_directory", std::string{"/"}}
          },
          std::vector<config::SettingPatch>{
              {"state", "directory", std::string{"sync/state"}}
          },
          std::vector<config::SettingPatch>{
              {"sync", "data_directory", std::string{"state/child"}}
          },
          std::vector<config::SettingPatch>{
              {"download", "validation", std::string{"unknown"}}
          },
          std::vector<config::SettingPatch>{
              {"sync", "future_option", std::int64_t{1}}
          },
          std::vector<config::SettingPatch>{
              {"upload", "concurrency", std::string{"not-an-integer"}}
          }}) {
        if (!test::throws_with([&] {
                static_cast<void>(gui::save_settings_patch(file, patches));
            }) ||
            test::read_file(file) != original || backups(fixture.path()) != 0) {
            throw std::runtime_error(
                "invalid typed patch changed file or made a backup: " +
                patches.front().table + "." + patches.front().key
            );
        }
    }
    const auto unknown = original + "future_option = 'keep unknown' # keep\n";
    test::write_file(file, unknown);
    require(
        test::throws_with([&] {
            static_cast<void>(gui::save_settings_patch(
                file, {{"auth", "application_id", std::string{"client"}}}
            ));
        }) &&
            test::read_file(file) == unknown && backups(fixture.path()) == 0,
        "unsupported TOML key was silently dropped instead of preserved"
    );
    const auto missing = fixture.path() / "not-created" / "config.toml";
    require(
        test::throws_with([&] {
            static_cast<void>(gui::save_settings_patch(
                missing, {{"state", "directory", std::string{""}}}
            ));
        }) &&
            !std::filesystem::exists(missing.parent_path()),
        "invalid first save created configuration directories"
    );
    const auto blocked = file / "config.toml";
    bool missing_flag = false;
    require(
        test::throws_with([&] {
            static_cast<void>(gui::load_configuration(blocked, missing_flag));
        }) &&
            test::throws_with([&] {
                static_cast<void>(gui::save_settings_patch(blocked, {}));
            }) &&
            test::read_file(file) == unknown && backups(fixture.path()) == 0,
        "non-directory configuration parent was treated as missing"
    );
}

void test_cancel_save_and_import_errors() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto source = fixture.path() / "import.toml";
    const std::string original =
        "config_version = 2\n[sync]\ndata_directory = 'sync'\n"
        "[state]\ndirectory = 'state'\n[auth]\napplication_id = 'client'\n";
    test::write_file(file, original);
    test::write_file(source, original);
    gui::MainWindow window{gui::load_app_state_view_model(file)};
    window.show();
    text(window, "auth.application_id", "unsaved-client");
    text(window, "sync.data_directory", "new-sync");
    QString message;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message = dialog->text();
            dialog->button(QMessageBox::Cancel)->click();
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("saveSettingsButton")->click();
    dialogs.stop();
    require(
        message.contains("Existing files") &&
            test::read_file(file) == original && backups(fixture.path()) == 0 &&
            window.findChild<QLineEdit*>("auth.application_id")->text() ==
                "unsaved-client",
        "cancelled directory change wrote configuration or reset drafts"
    );
    for (const int scenario : {0, 1, 2}) {
        if (scenario == 2) {
            test::write_file(source, "[broken TOML");
        }
        int stage = 0;
        QString result;
        QTimer import_dialogs;
        QObject::connect(&import_dialogs, &QTimer::timeout, &window, [&] {
            auto* modal = QApplication::activeModalWidget();
            if (auto* dialog = qobject_cast<QFileDialog*>(modal);
                dialog && stage == 0) {
                ++stage;
                if (scenario == 0) {
                    dialog->reject();
                } else {
                    dialog->selectFile(QString::fromStdString(source.string()));
                    QMetaObject::invokeMethod(
                        dialog, "accept", Qt::QueuedConnection
                    );
                }
            } else if (auto* dialog = qobject_cast<QMessageBox*>(modal)) {
                ++stage;
                result = dialog->text();
                if (dialog->standardButtons().testFlag(QMessageBox::Cancel)) {
                    dialog->button(QMessageBox::Cancel)->click();
                } else {
                    dialog->accept();
                }
            }
        });
        import_dialogs.start(1);
        window.findChild<QPushButton*>("importSettingsButton")->click();
        import_dialogs.stop();
        require(
            stage == (scenario == 0 ? 1 : 2) &&
                test::read_file(file) == original &&
                backups(fixture.path()) == 0 &&
                window.findChild<QLineEdit*>("auth.application_id")->text() ==
                    "unsaved-client",
            "cancelled/invalid import wrote configuration or reset drafts"
        );
        if (scenario == 2) {
            require(
                result.contains("could not be imported") &&
                    result.contains("cannot parse TOML"),
                "invalid import did not expose the parse error"
            );
        }
    }
}

void test_import_creates_missing_configuration() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "active" / "config.toml";
    const auto source = fixture.path() / "source" / "import.toml";
    std::filesystem::create_directory(source.parent_path());
    const std::string contents =
        "# import unchanged\nconfig_version = 2\n[sync]\n"
        "data_directory = '" +
        (file.parent_path() / "sync").string() +
        "'\nsync_list = 'rules.txt'\n[state]\ndirectory = '" +
        (file.parent_path() / "state").string() +
        "'\n[auth]\napplication_id = 'import-client'\n"
        "scopes = ['User.Read', 'offline_access']\n"
        "[proxy]\nno_proxy = ['.internal.test']\n"
        "[transfer]\nhttp_version = '1.1'\n[monitor]\npoll_interval_seconds = "
        "17\n"
        "[logging]\nlevel = 'warn'\n[filesystem]\nmetadata = 'database'\n";
    test::write_file(source, contents);
    gui::MainWindow window{gui::load_app_state_view_model(file)};
    text(window, "auth.application_id", "discarded-draft");
    text(window, "upload.concurrency", "13");
    choice(window, "sync.dotfiles", "exclude");
    text(window, "transfer.connect_timeout_seconds", "5");
    text(window, "monitor.poll_interval_seconds", "6");
    choice(window, "logging.level", "off");
    window.show();
    int stage = 0;
    QString preview;
    QString result;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (auto* dialog = qobject_cast<QFileDialog*>(modal);
            dialog && stage == 0) {
            ++stage;
            dialog->selectFile(QString::fromStdString(source.string()));
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        } else if (auto* dialog = qobject_cast<QMessageBox*>(modal)) {
            ++stage;
            if (stage == 2) {
                preview = dialog->text();
                dialog->button(QMessageBox::Yes)->click();
            } else {
                result = dialog->text();
                dialog->accept();
            }
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("importSettingsButton")->click();
    dialogs.stop();
    const auto loaded = config::Config::load(file);
    require(
        stage == 3 && test::read_file(file) == contents &&
            backups(file.parent_path()) == 0 &&
            result == "Configuration imported." &&
            preview.contains(
                QString::fromStdString((file.parent_path() / "sync").string())
            ) &&
            loaded.sync_list == file.parent_path() / "rules.txt" &&
            window.findChild<QLineEdit*>("auth.application_id")->text() ==
                "import-client" &&
            window.findChild<QLineEdit*>("upload.concurrency")->text() == "1" &&
            window.findChild<QComboBox*>("sync.dotfiles")->currentData() ==
                "include" &&
            window.findChild<QComboBox*>("transfer.http_version")
                    ->currentData() == "1.1" &&
            window.findChild<QLineEdit*>("monitor.poll_interval_seconds")
                    ->text() == "17" &&
            window.findChild<QComboBox*>("logging.level")->currentData() ==
                "warn" &&
            window.findChild<QComboBox*>("filesystem.metadata")
                    ->currentData() == "database" &&
            !window.findChild<QCheckBox*>("proxy.url.override")->isChecked() &&
            window.findChild<QCheckBox*>("proxy.no_proxy.override")
                ->isChecked() &&
            window.findChild<gui::SettingsEditor*>()->patches().empty() &&
            window.findChild<QPushButton*>("saveSettingsButton")->isEnabled(),
        "first import lost source, created backup, resolved relative paths "
        "from source or retained drafts"
    );
}

void test_endpoint_import_rejection() {
    test::TemporaryDirectory fixture;
    const auto active = fixture.path() / "config.toml";
    const auto incoming = fixture.path() / "import.toml";
    const std::string original =
        "config_version = 2\n[sync]\ndata_directory = 'sync'\n"
        "[state]\ndirectory = 'state'\n";
    test::write_file(active, original);
    for (const auto* unsafe :
         {"[auth]\nendpoint = 'http://login.example.test'\n",
          "[graph]\nendpoint = 'https://user:secret@graph.example.test'\n",
          "[proxy]\nurl = 'http://user:secret@proxy.example.test'\n",
          "[proxy]\nurl = 'https://proxy.example.test?secret=x'\n",
          "[auth]\nendpoint = 'https://[invalid'\n",
          "[auth]\nendpoint = 'https://login.example.test/#fragment'\n",
          "[graph]\nendpoint = 'https://user@graph.example.test'\n",
          "[graph]\nendpoint = \"https://graph.example.test/\\u0000\"\n",
          "[proxy]\npassword_file = 'private-password'\n",
          "[proxy]\nca_file = 'ca.pem'\n"}) {
        const auto source = original + unsafe;
        test::write_file(incoming, source);
        require(
            test::throws_with([&] {
                static_cast<void>(gui::preview_configuration(incoming, active));
            }) &&
                test::throws_with([&] {
                    static_cast<void>(
                        gui::import_configuration(incoming, active)
                    );
                }) &&
                test::read_file(active) == original &&
                test::read_file(incoming) == source &&
                backups(fixture.path()) == 0,
            "unsafe endpoint/file-reference import wrote a file or backup"
        );
    }
}

void test_phase_two_proxy_drafts_and_empty_save() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const std::string original =
        "config_version = 2\n[sync]\ndata_directory = 'sync'\n"
        "[state]\ndirectory = 'state'\n"
        "[proxy] # retained inheritance\nurl = 'https://proxy.example.test'\n";
    test::write_file(file, original);
    gui::SettingsEditor editor;
    for (const auto* retained : {"proxy.username", "proxy.auth"}) {
        editor.load(config::Config::load(file));
        checked(editor, "proxy.url.override", false);
        if (std::string_view{retained} == "proxy.username") {
            text(editor, retained, "inherited-user");
        } else {
            choice(editor, retained, "basic");
        }
        const auto patches = editor.patches();
        require(
            std::none_of(
                patches.begin(),
                patches.end(),
                [](const auto& patch) {
                    return patch.table == "proxy" && patch.key.empty();
                }
            ),
            "disabling URL discarded independent proxy credentials/auth"
        );
        const auto updated = config::update_settings(file, original, patches);
        const auto c = config::Config::load_from_string(file, updated);
        require(
            !c.proxy.url &&
                (c.proxy.username == "inherited-user" ||
                 c.proxy.auth == http::ProxyAuth::basic) &&
                updated.contains("# retained inheritance"),
            "retained proxy draft failed actual TOML reload"
        );
    }
    editor.load(config::Config::load(file));
    text(editor, "proxy.url", "");
    checked(editor, "proxy.no_proxy.override", true);
    list(editor, "auth.scopes", "");
    const auto backup = gui::save_settings_patch(file, editor.patches());
    const auto c = config::Config::load(file);
    require(
        backup && test::read_file(*backup) == original && c.proxy.url == "" &&
            c.proxy.no_proxy == std::vector<std::string>{} &&
            c.auth_scope.empty() && backups(fixture.path()) == 1,
        "GUI save collapsed explicit empty overrides or scope arrays"
    );
    editor.load(c);
    checked(editor, "proxy.url.override", false);
    checked(editor, "proxy.no_proxy.override", false);
    text(editor, "proxy.url", "https://discarded.example.test");
    list(editor, "proxy.no_proxy", "discarded.example.test");
    const auto unset = config::Config::load_from_string(
        file,
        config::update_settings(file, test::read_file(file), editor.patches())
    );
    require(
        !unset.proxy.url && !unset.proxy.no_proxy,
        "disabled proxy draft text persisted instead of removal"
    );
}

} // namespace onedrive::test::gui_settings
