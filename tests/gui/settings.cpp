#include "configuration.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "settings_editor.hpp"
#include "support/common.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <atomic>
namespace {
using namespace onedrive;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t backups(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().contains(".backup.")) {
            ++count;
        }
    }
    return count;
}

void text(QWidget& window, const char* name, const QString& value) {
    auto* field = window.findChild<QLineEdit*>(name);
    require(field != nullptr, "text field missing");
    field->setText(value);
}

void choice(QWidget& window, const char* name, const char* value) {
    auto* combo = window.findChild<QComboBox*>(name);
    require(
        combo != nullptr && combo->findData(QString::fromUtf8(value)) >= 0,
        "enum field missing"
    );
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(value)));
}

bool same_patches(
    const std::vector<config::SettingPatch>& left,
    const std::vector<config::SettingPatch>& right
) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].table != right[i].table || left[i].key != right[i].key ||
            left[i].value != right[i].value) {
            return false;
        }
    }
    return true;
}

QString save(gui::MainWindow& window) {
    QString message;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message = dialog->text();
            if (dialog->standardButtons().testFlag(QMessageBox::Save)) {
                dialog->button(QMessageBox::Save)->click();
            } else {
                dialog->accept();
            }
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("saveSettingsButton")->click();
    dialogs.stop();
    require(!message.isEmpty(), "save did not display a result");
    return message;
}

void test_round_trip() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto sync = fixture.path() / "sync";
    const auto state = fixture.path() / "state";
    const auto original = std::string{
        "# 中文 comment before\nconfig_version = 2\n"
        "[auth] # auth header\napplication_id = 'old-client' # retain auth\n"
        "[sync] # rules header\nmode = 'bidirectional' # retain mode\n"
        "sync_list = 'old-rules' # retain rules\n"
        "[console]\ntheme = 'hacker' # CLI only\n"
        "[monitor]\npoll_interval_seconds = 432\n"
        "[graph]\nendpoint = 'https://graph.microsoft.com/v1.0'\n"
        "[download]\nconcurrency = 4 # retain concurrency\n"
    };
    test::write_file(file, original);
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(file), {}, &localization
    };
    window.show();
    auto* tabs = window.findChild<QTabWidget*>("settingsTabs");
    require(tabs && tabs->count() == 3, "phase-one tabs are missing");
    text(window, "sync.data_directory", QString::fromStdString(sync.string()));
    text(window, "state.directory", QString::fromStdString(state.string()));
    text(window, "auth.application_id", "client-中文\"\\");
    text(window, "auth.tenant_id", "organizations");
    text(window, "sync.drive_id", "drive-中文");
    choice(window, "sync.mode", "download_only");
    choice(window, "sync.delete_policy", "preserve");
    choice(window, "sync.local_conflict", "backup");
    text(window, "sync.sync_list", "new-rules");
    choice(window, "sync.dotfiles", "exclude");
    text(window, "sync.maximum_file_size_bytes", "9223372036854775807");
    window.findChild<QCheckBox*>("sync.nosync_enabled")->setChecked(false);
    window.findChild<QCheckBox*>("sync.sync_root_files")->setChecked(true);
    text(window, "sync.maximum_remote_deletions", "0");
    text(window, "upload.concurrency", "16");
    text(window, "download.concurrency", "9");
    text(window, "upload.maximum_rate_bytes_per_second", "4294967297");
    text(window, "download.maximum_rate_bytes_per_second", "4294967298");
    text(window, "upload.maximum_total_rate_bytes_per_second", "4294967299");
    text(window, "download.maximum_total_rate_bytes_per_second", "4294967300");
    choice(window, "transfer.order", "name_dsc");
    text(window, "upload.chunk_size_bytes", "327680");
    text(window, "download.chunk_threshold_bytes", "4294967301");
    text(window, "download.checkpoint_interval_bytes", "4294967302");
    text(window, "download.maximum_retries", "8");
    choice(window, "download.validation", "relaxed");
    const auto drafts = window.findChild<gui::SettingsEditor*>()->patches();
    require(
        drafts.size() == 28,
        "all 26 fields and the two sync-mode compatibility patches expected"
    );
    for (int tab = 0; tab < 3; ++tab) {
        tabs->setCurrentIndex(tab);
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            tabs->tabText(0) == "账号与目录" &&
                tabs->tabText(1) == "同步规则" && tabs->tabText(2) == "传输",
            "tabs not translated"
        );
        require(
            window.findChild<QComboBox*>("transfer.order")->currentData() ==
                "name_dsc",
            "language switch changed enum data"
        );
        require(
            same_patches(
                drafts, window.findChild<gui::SettingsEditor*>()->patches()
            ),
            "Chinese translation changed draft values or patch types"
        );
        localization.set_language(gui::Language::english);
        QCoreApplication::processEvents();
    }
    require(
        same_patches(
            drafts, window.findChild<gui::SettingsEditor*>()->patches()
        ),
        "tab/language switches lost draft changes"
    );
    require(
        save(window).startsWith("Settings saved."), "round-trip save failed"
    );
    const auto config = config::Config::load(file);
    require(
        config.sync_data_directory == sync && config.state_directory == state &&
            config.application_id == "client-中文\"\\" &&
            config.azure_tenant_id == "organizations" &&
            config.drive_id == "drive-中文" &&
            config.sync_mode == sync::SyncMode::download_only &&
            config.delete_policy == sync::DeletePolicy::preserve &&
            config.local_conflict == config::LocalConflictPolicy::backup &&
            config.sync_list == fixture.path() / "new-rules" &&
            config.dotfiles == config::DotfilePolicy::exclude &&
            config.maximum_file_size_bytes == 9223372036854775807ULL &&
            !config.nosync_enabled && config.sync_root_files &&
            config.maximum_remote_deletions == 0 &&
            config.upload_concurrency == 16 &&
            config.download_concurrency == 9 &&
            config.upload_maximum_rate_bytes_per_second == 4294967297ULL &&
            config.download_maximum_rate_bytes_per_second == 4294967298ULL &&
            config.upload_maximum_total_rate_bytes_per_second ==
                4294967299ULL &&
            config.download_maximum_total_rate_bytes_per_second ==
                4294967300ULL &&
            config.transfer_order == config::TransferOrder::name_descending &&
            config.upload_chunk_size_bytes == 327680 &&
            config.download_chunk_threshold_bytes == 4294967301ULL &&
            config.download_checkpoint_interval_bytes == 4294967302ULL &&
            config.download_maximum_retries == 8 &&
            config.download_validation ==
                config::DownloadValidationMode::relaxed,
        "widget values failed actual TOML round trip"
    );
    const auto saved = test::read_file(file);
    require(
        saved.contains("# 中文 comment before") &&
            saved.contains("# auth header") &&
            saved.contains("# retain auth") &&
            saved.contains("# retain mode") &&
            saved.contains("# retain rules") &&
            saved.contains("# retain concurrency") &&
            saved.contains("[console]\ntheme = 'hacker' # CLI only\n") &&
            saved.contains("[monitor]\npoll_interval_seconds = 432\n") &&
            saved.contains(
                "[graph]\nendpoint = 'https://graph.microsoft.com/v1.0'\n"
            ),
        "save changed comments or unrelated configuration"
    );
    require(backups(fixture.path()) == 1, "save did not create one backup");
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.path())) {
        if (entry.path().filename().string().contains(".backup.")) {
            require(
                test::read_file(entry.path()) == original,
                "backup did not retain original"
            );
        }
    }
    for (const auto& [field, invalid] :
         {std::pair{"upload.concurrency", "17"},
          std::pair{"download.concurrency", "0"},
          std::pair{"upload.chunk_size_bytes", "1"},
          std::pair{"upload.chunk_size_bytes", "62914560"},
          std::pair{"download.chunk_threshold_bytes", "0"},
          std::pair{"download.checkpoint_interval_bytes", "0"},
          std::pair{"sync.maximum_file_size_bytes", "-1"},
          std::pair{
              "download.maximum_rate_bytes_per_second", "9223372036854775808"
          },
          std::pair{"download.maximum_retries", "1.5"},
          std::pair{"sync.maximum_remote_deletions", "abc"},
          std::pair{"sync.data_directory", ""},
          std::pair{"state.directory", ""}}) {
        auto* edit = window.findChild<QLineEdit*>(field);
        const auto valid = edit->text();
        edit->setText(invalid);
        require(
            !save(window).startsWith("Settings saved.") &&
                test::read_file(file) == saved && backups(fixture.path()) == 1,
            "invalid draft wrote configuration or backup"
        );
        edit->setText(valid);
    }
    text(window, "sync.sync_list", "");
    require(
        save(window).startsWith("Settings saved.") &&
            !config::Config::load(file).sync_list &&
            test::read_file(file).contains("# retain rules"),
        "clearing optional rules failed"
    );
    text(window, "auth.application_id", "unsaved");
    text(window, "upload.concurrency", "2");
    choice(window, "sync.mode", "upload_only");
    window.findChild<QPushButton*>("reloadSettingsButton")->click();
    require(
        window.findChild<QLineEdit*>("auth.application_id")->text() ==
                "client-中文\"\\" &&
            window.findChild<QLineEdit*>("upload.concurrency")->text() ==
                "16" &&
            window.findChild<QComboBox*>("sync.mode")->currentData() ==
                "download_only",
        "reload did not reset all draft fields"
    );
}

void test_source_editing() {
    const auto run = [](const std::string& source) {
        const auto updated = config::update_settings(
            "config.toml",
            source,
            {
                {"sync", "drive_id", std::string{"新 drive"}},
                {"sync", "maximum_file_size_bytes", std::int64_t{4294967297}},
                {"sync", "sync_list", std::monostate{}},
                {"upload", "concurrency", std::int64_t{2}},
                {"upload", "maximum_rate_bytes_per_second", std::int64_t{42}},
            }
        );
        const auto loaded =
            config::Config::load_from_string("config.toml", updated);
        require(
            loaded.drive_id == "新 drive" &&
                loaded.maximum_file_size_bytes == 4294967297ULL &&
                !loaded.sync_list && loaded.upload_concurrency == 2 &&
                loaded.upload_maximum_rate_bytes_per_second == 42,
            "typed patch failed source shape"
        );
        require(updated.contains("# keep"), "typed patch lost comment");
    };
    run("config_version = 2\n# keep\n[\"sync\"] # quoted header\nsync_list = "
        "'rules'\n");
    run("config_version = 2\n# keep\nsync.drive_id = 'old'\nsync.sync_list = "
        "'rules'\n");
    run("config_version = 2\n# keep\nsync = { drive_id = 'old', sync_list = "
        "'rules' }\n");
    run("config_version = 2\n# keep\n[sync]");
    run("config_version = 2\n# keep\n[auth]\napplication_id = "
        "'''text\n[sync]\nnot a header'''\n");
    const auto legacy = config::update_settings(
        "config.toml",
        "config_version = 2\n[sync]\nupload = false # keep legacy comment\n",
        {{"sync", "upload", std::monostate{}},
         {"sync", "mode", std::string{"upload_only"}},
         {"sync", "delete_policy", std::string{"propagate"}}}
    );
    require(
        config::Config::load_from_string("config.toml", legacy).sync_mode ==
                sync::SyncMode::upload_only &&
            legacy.contains("# keep legacy comment"),
        "legacy sync.upload was not safely replaced"
    );
    const auto implicit = config::update_settings(
        "config.toml",
        "config_version = 2\n[graph.throttle] # keep child header\n"
        "maximum_retries = 5\n",
        {{"graph", "endpoint", std::string{"https://example.test/v1.0"}}}
    );
    const auto implicit_config =
        config::Config::load_from_string("config.toml", implicit);
    require(
        implicit_config.graph_endpoint == "https://example.test/v1.0" &&
            implicit_config.graph_maximum_throttle_retries == 5 &&
            implicit.contains("# keep child header"),
        "typed patch misplaced an implicit parent setting"
    );
}

void test_first_setup() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    std::atomic<bool> proceed{false};
    std::atomic<bool> loaded_saved_id{false};
    gui::MainWindow window{
        gui::load_app_state_view_model(file),
        [&](
            const config::Config& saved, const auto&, std::stop_token stop
        ) -> auth::AuthResult<app::AuthenticationResult> {
            loaded_saved_id = saved.application_id == "first-client" &&
                              saved.upload_concurrency == 1;
            while (!proceed && !stop.stop_requested()) {
                QThread::msleep(1);
            }
            return app::AuthenticationResult{};
        }
    };
    require(!std::filesystem::exists(file), "startup wrote defaults");
    require(
        window.findChild<QLineEdit*>("auth.tenant_id")->text() == "common" &&
            window.findChild<QLineEdit*>("download.concurrency")->text() == "4",
        "missing configuration did not populate defaults"
    );
    text(window, "auth.application_id", "first-client");
    text(
        window,
        "sync.data_directory",
        QString::fromStdString((fixture.path() / "sync").string())
    );
    text(
        window,
        "state.directory",
        QString::fromStdString((fixture.path() / "state").string())
    );
    require(
        save(window).startsWith("Settings saved.") &&
            window.findChild<QPushButton*>("signInButton")->isEnabled() &&
            config::Config::load(file).application_id == "first-client" &&
            backups(fixture.path()) == 0,
        "first-time setup cannot sign in with saved Application ID"
    );
    text(window, "upload.concurrency", "12");
    window.findChild<QPushButton*>("signInButton")->click();
    require(
        !window.findChild<QWidget*>("settingsPage")->isEnabled() &&
            !window.findChild<QPushButton*>("saveSettingsButton")
                 ->isEnabled() &&
            !window.findChild<QPushButton*>("importSettingsButton")
                 ->isEnabled() &&
            !window.findChild<QPushButton*>("reloadSettingsButton")
                 ->isEnabled(),
        "login did not disable global settings controls"
    );
    proceed = true;
    require(
        test::wait_until(
            [&] {
                QCoreApplication::processEvents();
                return window.findChild<QWidget*>("settingsPage")->isEnabled();
            },
            std::chrono::seconds{5}
        ) && loaded_saved_id &&
            window.findChild<QLineEdit*>("upload.concurrency")->text() == "12",
        "login did not load saved ID or discarded the transfer draft"
    );
    const auto corrupt = std::string{"# keep corrupt\n[broken TOML"};
    test::write_file(file, corrupt);
    window.findChild<QPushButton*>("reloadSettingsButton")->click();
    require(
        !window.findChild<QPushButton*>("saveSettingsButton")->isEnabled() &&
            window.findChild<QPushButton*>("importSettingsButton")
                ->isEnabled() &&
            window.findChild<QPushButton*>("reloadSettingsButton")->isEnabled(),
        "invalid configuration recovery controls broken"
    );
    require(
        test::throws_with([&] {
            static_cast<void>(gui::save_settings_patch(
                file,
                {{"auth", "application_id", std::string{"must not overwrite"}}}
            ));
        }) &&
            test::read_file(file) == corrupt && backups(fixture.path()) == 0,
        "corrupt configuration was overwritten or backed up by ordinary save"
    );
}

void test_inline_removal_and_utf8() {
    for (const auto* fields :
         {"sync_list = 'rules', drive_id = '中文'",
          "drive_id = '中文', sync_list = 'rules'",
          "drive_id = '中文', sync_list = 'rules', dotfiles = 'exclude'",
          "sync_list = 'rules'",
          ""}) {
        const std::string source =
            "config_version = 2\r\nsync = { " + std::string{fields} +
            " } # 保留 inline\r\n[console]\ntheme = 'hacker' # keep CLI\n";
        const auto removed = config::update_settings(
            "config.toml", source, {{"sync", "sync_list", std::monostate{}}}
        );
        const auto loaded =
            config::Config::load_from_string("config.toml", removed);
        require(
            !loaded.sync_list && removed.contains("# 保留 inline\r\n") &&
                removed.contains("[console]\ntheme = 'hacker' # keep CLI\n"),
            "inline removal damaged siblings, comments or line endings"
        );
        if (std::string_view{fields}.contains("drive_id")) {
            require(loaded.drive_id == "中文", "inline removal lost sibling");
        }
        const auto added = config::update_settings(
            "config.toml",
            removed,
            {{"sync", "sync_list", std::string{"new-rules"}},
             {"sync", "drive_id", std::string{"新😀\"\\drive"}},
             {"sync", "nosync_enabled", false}}
        );
        const auto reloaded =
            config::Config::load_from_string("config.toml", added);
        require(
            reloaded.sync_list == "new-rules" &&
                reloaded.drive_id == "新😀\"\\drive" &&
                !reloaded.nosync_enabled && added.contains("# 保留 inline\r\n"),
            "inline insertion/replacement failed typed UTF-8 round trip"
        );
        require(
            config::update_settings("config.toml", added, {}) == added,
            "empty typed patch rewrote original source"
        );
    }
    const std::string quoted =
        "config_version = 2\n[sync]\n\"drive_id\" = '中文😀' # keep\n"
        "sync_list = '''多行\nrules''' # keep rules\n";
    const auto updated = config::update_settings(
        "config.toml",
        quoted,
        {{"sync", "drive_id", std::string{"replacement"}},
         {"sync", "sync_list", std::monostate{}},
         {"sync", "sync_list", std::monostate{}}}
    );
    require(
        config::Config::load_from_string("config.toml", updated).drive_id ==
                "replacement" &&
            updated.contains("# keep\n") && updated.contains("# keep rules"),
        "quoted key/multiline optional removal lost comments"
    );
}

void test_editor_enum_reload_and_numeric_syntax() {
    gui::SettingsEditor editor;
    for (const auto* order :
         {"default", "size_asc", "size_dsc", "name_asc", "name_dsc"}) {
        for (const auto* mode :
             {"bidirectional", "upload_only", "download_only"}) {
            const auto loaded = config::Config::load_from_string(
                "config.toml",
                "config_version = 2\n[sync]\nmode = '" + std::string{mode} +
                    "'\n[transfer]\norder = '" + order + "'\n"
            );
            editor.load(loaded);
            require(
                editor.findChild<QComboBox*>("transfer.order")->currentData() ==
                        order &&
                    editor.findChild<QComboBox*>("sync.mode")->currentData() ==
                        mode &&
                    editor.patches().empty(),
                "enum reload created a spurious edit or lost canonical data"
            );
        }
    }
    editor.load(config::Config::defaults());
    for (const auto* invalid :
         {"",
          "+1",
          "-0",
          " 1",
          "1 ",
          "1\n",
          "１",
          "١",
          "1e3",
          "18446744073709551615",
          "9223372036854775808"}) {
        text(editor, "download.maximum_retries", invalid);
        require(
            test::throws_with(
                [&] { static_cast<void>(editor.patches()); },
                "download.maximum_retries"
            ),
            "noncanonical/out-of-range integer accepted or field not identified"
        );
    }
    for (const auto* valid : {"0", "0001", "9223372036854775807"}) {
        text(editor, "download.maximum_retries", valid);
        const auto patches = editor.patches();
        require(
            patches.size() == 1 &&
                std::get<std::int64_t>(patches.front().value) ==
                    QString{valid}.toLongLong(),
            "valid nonnegative int64 draft was truncated or mistyped"
        );
    }
    editor.load(config::Config::defaults());
    choice(editor, "sync.mode", "upload_only");
    auto source = config::update_settings(
        "config.toml", "config_version = 2\n", editor.patches()
    );
    require(
        config::Config::load_from_string("config.toml", source).delete_policy ==
            sync::DeletePolicy::propagate,
        "upload-only edit changed unchanged deletion policy implicitly"
    );
    editor.load(config::Config::load_from_string("config.toml", source));
    choice(editor, "sync.mode", "bidirectional");
    source = config::update_settings("config.toml", source, editor.patches());
    require(
        config::Config::load_from_string("config.toml", source).sync_mode ==
                sync::SyncMode::bidirectional &&
            !source.contains("upload ="),
        "switching back from upload-only retained a legacy mode"
    );
}

void test_numeric_limits_round_trip() {
    gui::SettingsEditor editor;
    const auto defaults = config::Config::defaults();
    for (const bool maximum : {false, true}) {
        editor.load(defaults);
        for (const auto* name :
             {"sync.maximum_file_size_bytes",
              "sync.maximum_remote_deletions",
              "upload.maximum_rate_bytes_per_second",
              "download.maximum_rate_bytes_per_second",
              "upload.maximum_total_rate_bytes_per_second",
              "download.maximum_total_rate_bytes_per_second",
              "download.maximum_retries"}) {
            text(editor, name, maximum ? "9223372036854775807" : "0");
        }
        for (const auto* name :
             {"download.chunk_threshold_bytes",
              "download.checkpoint_interval_bytes"}) {
            text(editor, name, maximum ? "9223372036854775807" : "1");
        }
        text(editor, "upload.concurrency", maximum ? "16" : "1");
        text(editor, "download.concurrency", maximum ? "16" : "1");
        text(
            editor, "upload.chunk_size_bytes", maximum ? "62586880" : "327680"
        );
        const auto patches = editor.patches();
        const auto updated = config::update_settings(
            "config.toml", "config_version = 2\n", patches
        );
        const auto loaded =
            config::Config::load_from_string("config.toml", updated);
        const auto expected = maximum ? 9223372036854775807ULL : 0ULL;
        require(
            loaded.maximum_file_size_bytes == expected &&
                loaded.maximum_remote_deletions == expected &&
                loaded.upload_maximum_rate_bytes_per_second == expected &&
                loaded.download_maximum_rate_bytes_per_second == expected &&
                loaded.upload_maximum_total_rate_bytes_per_second == expected &&
                loaded.download_maximum_total_rate_bytes_per_second ==
                    expected &&
                loaded.download_maximum_retries == expected &&
                loaded.download_chunk_threshold_bytes ==
                    (maximum ? expected : 1ULL) &&
                loaded.download_checkpoint_interval_bytes ==
                    (maximum ? expected : 1ULL) &&
                loaded.upload_chunk_size_bytes ==
                    (maximum ? 62586880ULL : 327680ULL),
            "numeric lower/upper boundary changed during typed TOML round trip"
        );
        editor.load(loaded);
        require(editor.patches().empty(), "boundary reload created drafts");
        for (const auto& patch : patches) {
            const auto name =
                QString::fromStdString(patch.table + "." + patch.key);
            require(
                editor.findChild<QLineEdit*>(name)->text() ==
                    QString::number(std::get<std::int64_t>(patch.value)),
                "numeric display truncated a saved boundary"
            );
        }
    }
}

void test_editor_browse_and_advanced() {
    test::TemporaryDirectory fixture;
    const auto directory = fixture.path() / "chosen";
    const auto rules = fixture.path() / "rules.txt";
    std::filesystem::create_directory(directory);
    test::write_file(rules, "+ keep\n");
    gui::SettingsEditor editor;
    editor.load(config::Config::defaults());
    editor.show();
    auto* tabs = editor.findChild<QTabWidget*>("settingsTabs");
    for (int i = 0; i < 2; ++i) {
        tabs->setCurrentIndex(i + 1);
        auto* toggle = tabs->widget(i + 1)->findChild<QToolButton*>();
        require(toggle && toggle->isCheckable(), "advanced group missing");
        auto* field = editor.findChild<QLineEdit*>(
            i == 0 ? "sync.maximum_remote_deletions" : "upload.chunk_size_bytes"
        );
        require(!field->isVisible(), "advanced group starts expanded");
        toggle->click();
        require(
            field->isVisible() && toggle->arrowType() == Qt::DownArrow,
            "expanding advanced group did not reveal settings"
        );
        toggle->click();
        require(
            !field->isVisible() && toggle->arrowType() == Qt::RightArrow &&
                editor.patches().empty(),
            "collapsing advanced group changed a draft"
        );
    }
    for (const auto* name :
         {"sync.data_directory", "state.directory", "sync.sync_list"}) {
        tabs->setCurrentIndex(
            std::string_view{name} == "sync.sync_list" ? 1 : 0
        );
        auto* edit = editor.findChild<QLineEdit*>(name);
        const auto original = edit->text();
        auto* browse =
            edit->parentWidget()->findChild<QPushButton*>("browseButton");
        require(browse != nullptr, "browse button missing");
        for (const bool accept : {false, true}) {
            bool handled = false;
            QTimer dialogs;
            QObject::connect(&dialogs, &QTimer::timeout, &editor, [&] {
                if (auto* dialog = qobject_cast<QFileDialog*>(
                        QApplication::activeModalWidget()
                    );
                    dialog && !handled) {
                    handled = true;
                    if (accept) {
                        const auto target =
                            std::string_view{name} == "sync.sync_list"
                                ? rules
                                : directory;
                        dialog->selectFile(
                            QString::fromStdString(target.string())
                        );
                        QMetaObject::invokeMethod(
                            dialog, "accept", Qt::QueuedConnection
                        );
                    } else {
                        dialog->reject();
                    }
                }
            });
            dialogs.start(1);
            browse->click();
            dialogs.stop();
            require(handled, "browse did not open a file dialog");
            const auto expected =
                std::string_view{name} == "sync.sync_list" ? rules : directory;
            require(
                edit->text() == (accept
                                     ? QString::fromStdString(expected.string())
                                     : original),
                "browse cancellation changed draft or selection was lost"
            );
        }
    }
}

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
        "'\n[auth]\napplication_id = 'import-client'\n";
    test::write_file(source, contents);
    gui::MainWindow window{gui::load_app_state_view_model(file)};
    text(window, "auth.application_id", "discarded-draft");
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
            window.findChild<QPushButton*>("saveSettingsButton")->isEnabled(),
        "first import lost source, created backup, resolved relative paths "
        "from source or retained drafts"
    );
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    try {
        test_source_editing();
        test_round_trip();
        test_first_setup();
        test_inline_removal_and_utf8();
        test_editor_enum_reload_and_numeric_syntax();
        test_numeric_limits_round_trip();
        test_editor_browse_and_advanced();
        test_persistence_rejects_without_writes();
        test_cancel_save_and_import_errors();
        test_import_creates_missing_configuration();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
