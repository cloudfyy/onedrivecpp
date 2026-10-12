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
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <atomic>
#include <cstdint>
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

void list(QWidget& window, const char* name, const QString& value) {
    auto* field = window.findChild<QPlainTextEdit*>(name);
    require(field != nullptr, "list field missing");
    field->setPlainText(value);
}

void checked(QWidget& window, const char* name, bool value) {
    auto* field = window.findChild<QCheckBox*>(name);
    require(field != nullptr, "checkbox field missing");
    field->setChecked(value);
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

void test_tooltips_and_language_switching() {
    test::TemporaryDirectory fixture;
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.path() / "config.toml"),
        {},
        &localization
    };
    auto* editor = window.findChild<gui::SettingsEditor*>();
    require(editor != nullptr, "settings editor missing");
    std::vector<std::pair<QWidget*, QString>> tips;
    const auto remember = [&](QWidget* widget, const QString& expected) {
        require(
            widget->toolTip() == expected &&
                widget->accessibleDescription() == expected,
            "tooltip/accessibility text missing on field or companion"
        );
        tips.emplace_back(widget, expected);
    };
    std::size_t inputs = 0;
    for (auto* label : editor->findChildren<QLabel*>()) {
        auto* input = label->buddy();
        if (!input) {
            continue;
        }
        ++inputs;
        const auto tip = input->toolTip();
        require(
            tip.size() >= 30 && tip.size() <= 200 && tip != label->text(),
            "setting tip should be concise, meaningful and not just its label"
        );
        remember(input, tip);
        remember(label, tip);
        if (auto* browse = input->parentWidget()->findChild<QPushButton*>(
                "browseButton", Qt::FindDirectChildrenOnly
            )) {
            remember(browse, tip);
            remember(input->parentWidget(), tip);
        }
        if (auto* override = editor->findChild<QCheckBox*>(
                input->objectName() + ".override"
            )) {
            remember(override, tip);
            remember(input->parentWidget(), tip);
        }
    }
    require(inputs == 57, "not all 57 settings inputs have tips");
    std::size_t controls = 0;
    for (auto* widget : editor->findChildren<QWidget*>()) {
        if (widget->objectName().contains('.') &&
            (qobject_cast<QLineEdit*>(widget) ||
             qobject_cast<QPlainTextEdit*>(widget) ||
             qobject_cast<QComboBox*>(widget) ||
             qobject_cast<QCheckBox*>(widget))) {
            ++controls;
            require(!widget->toolTip().isEmpty(), "uncovered settings control");
        }
    }
    require(controls == 59, "settings inputs or override controls missed");
    const auto explains = [&](const char* name, const QStringList& fragments) {
        const auto* widget = editor->findChild<QWidget*>(name);
        require(widget != nullptr, "representative setting missing");
        for (const auto& fragment : fragments) {
            require(
                widget->toolTip().contains(fragment),
                "setting tip lost essential semantics/default/units"
            );
        }
    };
    explains(
        "transfer.connect_timeout_seconds", {"30 seconds", "upload/download"}
    );
    explains("monitor.settle_delay_milliseconds", {"1000 milliseconds", "0"});
    explains(
        "transfer.stall_minimum_bytes_per_second",
        {"1 byte/second", "timeout is 0"}
    );
    explains("upload.chunk_size_bytes", {"10485760 bytes", "250000000"});
    explains("download.chunk_threshold_bytes", {"8388608 bytes", "relaxed"});
    explains("upload.concurrency", {"files", "default: 1", "sequential"});
    explains("download.concurrency", {"default: 4", "serialized"});
    explains(
        "upload.maximum_rate_bytes_per_second",
        {"Each", "not the combined", "0 (unlimited)"}
    );
    explains(
        "download.maximum_total_rate_bytes_per_second",
        {"all active downloads", "individual"}
    );
    explains(
        "sync.maximum_remote_deletions", {"1000", "descendants", "0 blocks"}
    );
    explains("sync.delete_policy", {"deletes", "dry run", "preserve"});
    explains("sync.local_conflict", {"safeBackup", "replacing", "extra space"});
    explains(
        "download.validation",
        {"Strict (default)",
         "all downloads",
         "weakening integrity",
         "resume/chunking"}
    );
    explains(
        "proxy.url", {"Unchecked", "environment", "empty URL forces direct"}
    );
    explains(
        "proxy.no_proxy", {"NO_PROXY/no_proxy", "independently", "empty list"}
    );
    explains(
        "sync.sync_list",
        {"unset selects all", "empty rules file selects nothing", "retained"}
    );
    explains(
        "sync.data_mount_point", {"missing-storage deletions", "empty disables"}
    );
    explains("proxy.password_file", {"owner-only", "no symlinks", "64 KiB"});
    explains(
        "proxy.ca_file",
        {"HTTPS proxy", "system trust", "verification stays enabled"}
    );
    explains("auth.scopes", {"offline_access", "signing in again"});
    explains("auth.endpoint", {"HTTPS", "chinacloudapi.cn", "Match"});
    explains(
        "graph.endpoint",
        {"receiving tokens", "chinacloudapi.cn/v1.0", "not a SharePoint"}
    );
    explains("state.directory", {"Private", "do not migrate", "sign in again"});
    explains(
        "sync.permissions", {"0600", "0700", "umask", "state stays private"}
    );
    explains("filesystem.metadata", {"always uses SQLite", "requires support"});
    explains(
        "graph.throttle.maximum_delay_seconds", {"300 seconds", "stops retries"}
    );
    require(editor->patches().empty(), "tips changed baseline settings");

    auto* tabs = editor->findChild<QTabWidget*>("settingsTabs");
    for (const auto& [tab, field] : {
             std::pair{0, "auth.scopes"},
             std::pair{1, "sync.maximum_remote_deletions"},
             std::pair{2, "upload.chunk_size_bytes"},
             std::pair{3, "graph.throttle.maximum_delay_seconds"},
             std::pair{4, "monitor.websocket_renewal_lead_seconds"},
         }) {
        tabs->setCurrentIndex(tab);
        require(
            editor->findChild<QWidget*>(field)->isHidden() ||
                !editor->findChild<QWidget*>(field)->isVisibleTo(
                    tabs->widget(tab)
                ),
            "advanced settings should start collapsed"
        );
    }
    text(window, "auth.application_id", "draft-client");
    text(window, "upload.chunk_size_bytes", "327680");
    list(
        window, "auth.scopes", "User.Read\nFiles.ReadWrite.All\noffline_access"
    );
    checked(window, "sync.nosync_enabled", false);
    checked(window, "proxy.url.override", true);
    text(window, "proxy.url", "");
    checked(window, "proxy.no_proxy.override", true);
    list(window, "proxy.no_proxy", "");
    choice(window, "download.validation", "relaxed");
    const auto draft = editor->patches();
    const auto assert_draft = [&] {
        require(
            same_patches(draft, editor->patches()),
            "tooltip retranslation changed draft values or optional presence"
        );
    };
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(localization.chinese_loaded(), "Chinese translation did not load");
    for (const auto& [widget, english] : tips) {
        require(
            !widget->toolTip().isEmpty() && widget->toolTip() != english &&
                widget->accessibleDescription() == widget->toolTip(),
            "field/label/companion tip did not retranslate to Chinese"
        );
    }
    require(
        editor->findChild<QWidget*>("proxy.no_proxy")
                ->toolTip()
                .contains("独立继承 NO_PROXY/no_proxy") &&
            editor->findChild<QWidget*>("download.validation")
                ->toolTip()
                .contains("降低完整性保证"),
        "essential Chinese tip semantics missing"
    );
    assert_draft();
    localization.set_language(gui::Language::english);
    QCoreApplication::processEvents();
    for (const auto& [widget, english] : tips) {
        require(
            widget->toolTip() == english &&
                widget->accessibleDescription() == english,
            "tooltip did not switch back to English"
        );
    }
    assert_draft();
}

void test_tooltips_initial_locale_and_reload() {
    const auto config = config::Config::load_from_string(
        "config.toml", "config_version = 2\n[proxy]\nurl = ''\nno_proxy = []\n"
    );
    std::vector<std::pair<QString, QString>> english_tips;
    for (const auto* locale : {"en_US", "zh_CN", "fr_FR", "zh_TW"}) {
        gui::Localization localization{QLocale{locale}};
        gui::SettingsEditor editor;
        editor.load(config);
        const bool chinese = std::string_view{locale} == "zh_CN";
        require(
            localization.chinese_loaded() == chinese,
            "initial tooltip locale did not load or fall back as expected"
        );
        for (auto* label : editor.findChildren<QLabel*>()) {
            auto* input = label->buddy();
            if (!input) {
                continue;
            }
            const auto tip = input->toolTip();
            if (english_tips.size() < 57 && !chinese) {
                english_tips.emplace_back(input->objectName(), tip);
            }
            const auto original = std::find_if(
                english_tips.begin(),
                english_tips.end(),
                [&](const auto& entry) {
                    return entry.first == input->objectName();
                }
            );
            require(
                original != english_tips.end() &&
                    (chinese ? tip != original->second
                             : tip == original->second),
                "initial Chinese or unsupported-locale tooltip is incorrect"
            );
            require(
                input->accessibleDescription() == tip &&
                    label->toolTip() == tip &&
                    label->accessibleDescription() == tip,
                "initial label/input accessibility description is incorrect"
            );
        }
        require(
            english_tips.size() == 57, "initial tooltip inventory incomplete"
        );
        for (const auto* name : {"proxy.url", "proxy.no_proxy"}) {
            auto* input = editor.findChild<QWidget*>(name);
            auto* toggle = editor.findChild<QCheckBox*>(
                QString::fromUtf8(name) + ".override"
            );
            require(
                toggle->isChecked() && input->isEnabled() &&
                    toggle->toolTip() == input->toolTip() &&
                    toggle->accessibleDescription() == input->toolTip(),
                "explicitly empty proxy override lost its initial tip or "
                "presence"
            );
        }
        const auto tip = editor.findChild<QWidget*>("proxy.url")->toolTip();
        editor.load(config::Config::defaults());
        editor.retranslate();
        require(
            editor.patches().empty(), "default reload created draft patches"
        );
        for (const auto* name : {"proxy.url", "proxy.no_proxy"}) {
            auto* input = editor.findChild<QWidget*>(name);
            auto* toggle = editor.findChild<QCheckBox*>(
                QString::fromUtf8(name) + ".override"
            );
            require(
                !toggle->isChecked() && !input->isEnabled() &&
                    !input->toolTip().isEmpty() &&
                    input->accessibleDescription() == input->toolTip() &&
                    toggle->toolTip() == input->toolTip() &&
                    toggle->accessibleDescription() == input->toolTip(),
                "inherited/disabled proxy control lost its accessible tip"
            );
        }
        require(
            editor.findChild<QWidget*>("proxy.url")->toolTip() == tip,
            "reloading defaults changed a setting's help text"
        );
    }
}

void test_tooltips_expanded_groups_and_disabled_drafts() {
    test::TemporaryDirectory fixture;
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.path() / "config.toml"),
        {},
        &localization
    };
    window.show();
    auto* editor = window.findChild<gui::SettingsEditor*>();
    auto* tabs = editor->findChild<QTabWidget*>("settingsTabs");
    text(window, "proxy.url", "https://draft.example.test");
    list(window, "proxy.no_proxy", "localhost\n.example.test");
    checked(window, "proxy.url.override", true);
    checked(window, "proxy.no_proxy.override", true);
    const auto enabled_draft = editor->patches();
    checked(window, "proxy.url.override", false);
    checked(window, "proxy.no_proxy.override", false);
    const auto disabled_draft = editor->patches();
    for (int tab = 0; tab < 5; ++tab) {
        tabs->setCurrentIndex(tab);
        auto* toggle = tabs->widget(tab)->findChild<QToolButton*>();
        require(toggle != nullptr, "advanced settings toggle missing");
        toggle->click();
        require(
            toggle->isChecked() && toggle->arrowType() == Qt::DownArrow,
            "advanced settings group did not expand"
        );
    }
    for (const auto language :
         {gui::Language::simplified_chinese, gui::Language::english}) {
        localization.set_language(language);
        QCoreApplication::processEvents();
        for (int tab = 0; tab < 5; ++tab) {
            tabs->setCurrentIndex(tab);
            auto* toggle = tabs->widget(tab)->findChild<QToolButton*>();
            require(
                toggle->isChecked() && toggle->arrowType() == Qt::DownArrow,
                "language switch collapsed an expanded advanced group"
            );
            for (auto* label : tabs->widget(tab)->findChildren<QLabel*>()) {
                auto* input = label->buddy();
                if (input) {
                    require(
                        input->isVisible() && !input->toolTip().isEmpty() &&
                            label->toolTip() == input->toolTip() &&
                            input->accessibleDescription() == input->toolTip(),
                        "expanded advanced field lost its "
                        "visible/accessibility tip"
                    );
                }
            }
        }
        require(
            same_patches(disabled_draft, editor->patches()) &&
                !editor->findChild<QWidget*>("proxy.url")->isEnabled() &&
                !editor->findChild<QWidget*>("proxy.no_proxy")->isEnabled() &&
                editor->findChild<QLineEdit*>("proxy.url")->text() ==
                    "https://draft.example.test" &&
                editor->findChild<QPlainTextEdit*>("proxy.no_proxy")
                        ->toPlainText() == "localhost\n.example.test",
            "language switch enabled or discarded an inactive proxy draft"
        );
    }
    checked(window, "proxy.url.override", true);
    checked(window, "proxy.no_proxy.override", true);
    require(
        same_patches(enabled_draft, editor->patches()),
        "re-enabling overrides did not restore the untranslated draft"
    );
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
    require(tabs && tabs->count() == 6, "six settings tabs are missing");
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
    for (int tab = 0; tab < 6; ++tab) {
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

void test_phase_two_round_trip() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const std::string original =
        "# keep phase two\nconfig_version = 2\n"
        "[sync]\ndata_directory = 'sync'\n[ state ]\ndirectory = 'state'\n"
        "[proxy] # keep proxy\nurl = '' # keep url\nno_proxy = [] # keep "
        "bypass\n"
        "[graph.throttle] # keep throttle\nmaximum_retries = 4 # keep retries\n"
        "[monitor] # keep monitor\nwebsocket_enabled = true # keep enabled\n"
        "[logging] # keep logging\nfile = 'old.log' # keep file\n"
        "[console]\ntheme = 'hacker' # untouched\n";
    test::write_file(file, original);
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        gui::load_app_state_view_model(file), {}, &localization
    };
    window.show();
    auto* editor = window.findChild<gui::SettingsEditor*>();
    text(window, "auth.endpoint", "https://login.example.test");
    text(window, "graph.endpoint", "https://graph.example.test/v1.0");
    list(
        window, "auth.scopes", "User.Read\nFiles.ReadWrite.All\noffline_access"
    );
    text(window, "sync.data_mount_point", "/mnt/OneDrive");
    text(window, "proxy.url", "https://proxy.example.test:8443");
    list(
        window, "proxy.no_proxy", "localhost\n.example.test\n127.0.0.0/8\n::1"
    );
    text(window, "proxy.username", "用户\"\\");
    text(window, "proxy.password_file", "secrets/password");
    choice(window, "proxy.auth", "negotiate");
    text(window, "proxy.ca_file", "certificates/ca.pem");
    text(window, "transfer.connect_timeout_seconds", "42");
    text(window, "transfer.operation_timeout_seconds", "1234");
    text(window, "transfer.stall_timeout_seconds", "0");
    text(
        window, "transfer.stall_minimum_bytes_per_second", "9223372036854775807"
    );
    choice(window, "transfer.http_version", "2");
    choice(window, "transfer.ip_version", "6");
    text(window, "graph.throttle.maximum_retries", "4294967297");
    text(window, "graph.throttle.initial_delay_seconds", "7");
    text(window, "graph.throttle.maximum_delay_seconds", "31");
    checked(window, "monitor.websocket_enabled", false);
    text(window, "monitor.poll_interval_seconds", "86400");
    text(window, "monitor.settle_delay_milliseconds", "60000");
    text(window, "monitor.websocket_request_timeout_seconds", "300");
    text(window, "monitor.websocket_connect_timeout_seconds", "299");
    text(window, "monitor.websocket_renewal_lead_seconds", "3600");
    text(window, "monitor.websocket_initial_backoff_seconds", "3600");
    text(window, "monitor.websocket_maximum_backoff_seconds", "86400");
    choice(window, "logging.level", "trace");
    text(window, "logging.file", "logs/中文.log");
    choice(window, "sync.permissions", "umask");
    choice(window, "filesystem.metadata", "database");
    const auto draft = editor->patches();
    require(draft.size() == 31, "phase two does not patch every new field");
    auto* tabs = window.findChild<QTabWidget*>("settingsTabs");
    for (int tab = 0; tab < 6; ++tab) {
        tabs->setCurrentIndex(tab);
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            tabs->tabText(3) == "网络" && tabs->tabText(4) == "监控" &&
                tabs->tabText(5) == "高级与日志" &&
                same_patches(draft, editor->patches()),
            "phase two language switch changed drafts or missed tabs"
        );
        localization.set_language(gui::Language::english);
        QCoreApplication::processEvents();
    }
    require(
        test::read_file(file) == original && backups(fixture.path()) == 0,
        "editing mutated persistent configuration"
    );
    require(
        save(window).startsWith("Settings saved."), "phase two save failed"
    );
    const auto c = config::Config::load(file);
    require(
        c.auth_endpoint == "https://login.example.test" &&
            c.graph_endpoint == "https://graph.example.test/v1.0" &&
            c.auth_scope == "User.Read Files.ReadWrite.All offline_access" &&
            c.sync_data_mount_point == "/mnt/OneDrive" &&
            c.proxy.url == "https://proxy.example.test:8443" &&
            c.proxy.no_proxy ==
                std::vector<std::string>{
                    "localhost", ".example.test", "127.0.0.0/8", "::1"
                } &&
            c.proxy.username == "用户\"\\" &&
            c.proxy.password_file == fixture.path() / "secrets/password" &&
            c.proxy.auth == http::ProxyAuth::negotiate &&
            c.proxy.ca_file == fixture.path() / "certificates/ca.pem" &&
            c.transfer_transport.connect_timeout.count() == 42 &&
            c.transfer_transport.operation_timeout.count() == 1234 &&
            c.transfer_transport.low_speed_timeout.count() == 0 &&
            c.transfer_transport.low_speed_limit_bytes_per_second ==
                9223372036854775807ULL &&
            c.transfer_transport.http_version == http::HttpVersion::http_2 &&
            c.transfer_transport.ip_version == http::IpVersion::ipv6 &&
            c.graph_maximum_throttle_retries == 4294967297ULL &&
            c.graph_initial_throttle_delay.count() == 7 &&
            c.graph_maximum_throttle_delay.count() == 31 &&
            !c.monitor_websocket_enabled &&
            c.monitor_poll_interval.count() == 86400 &&
            c.monitor_settle_delay.count() == 60000 &&
            c.monitor_websocket_request_timeout.count() == 300 &&
            c.monitor_websocket_connect_timeout.count() == 299 &&
            c.monitor_websocket_renewal_lead.count() == 3600 &&
            c.monitor_websocket_initial_backoff.count() == 3600 &&
            c.monitor_websocket_maximum_backoff.count() == 86400 &&
            c.logging.level == "trace" &&
            c.logging.file == fixture.path() / "logs/中文.log" &&
            c.sync_permissions == config::SyncPermissionsMode::umask &&
            c.filesystem_metadata == config::FilesystemMetadataMode::database &&
            editor->patches().empty(),
        "phase two Config round trip lost types, paths, units or precision"
    );
    const auto saved = test::read_file(file);
    for (const auto* comment :
         {"# keep phase two",
          "# keep proxy",
          "# keep url",
          "# keep bypass",
          "# keep throttle",
          "# keep retries",
          "# keep monitor",
          "# keep enabled",
          "# keep logging",
          "# keep file",
          "theme = 'hacker' # untouched"}) {
        require(
            saved.contains(comment),
            "phase two lost comments or unspecified keys"
        );
    }
    for (const auto& [field, invalid] :
         {std::pair{"transfer.connect_timeout_seconds", "0"},
          std::pair{"transfer.operation_timeout_seconds", "0"},
          std::pair{"transfer.stall_minimum_bytes_per_second", "0"},
          std::pair{"transfer.stall_timeout_seconds", "-1"},
          std::pair{"graph.throttle.initial_delay_seconds", "32"},
          std::pair{"graph.throttle.maximum_retries", "9223372036854775808"},
          std::pair{"monitor.poll_interval_seconds", "0"},
          std::pair{"monitor.poll_interval_seconds", "86401"},
          std::pair{"monitor.settle_delay_milliseconds", "60001"},
          std::pair{"monitor.websocket_request_timeout_seconds", "0"},
          std::pair{"monitor.websocket_request_timeout_seconds", "301"},
          std::pair{"monitor.websocket_connect_timeout_seconds", "0"},
          std::pair{"monitor.websocket_connect_timeout_seconds", "301"},
          std::pair{"monitor.websocket_renewal_lead_seconds", "3601"},
          std::pair{"monitor.websocket_initial_backoff_seconds", "0"},
          std::pair{"monitor.websocket_initial_backoff_seconds", "3601"},
          std::pair{"monitor.websocket_maximum_backoff_seconds", "3599"},
          std::pair{"monitor.websocket_maximum_backoff_seconds", "86401"},
          std::pair{"proxy.url", "http://proxy.example.test"},
          std::pair{"proxy.url", "https://user:secret@proxy.example.test"},
          std::pair{"proxy.url", "https://"},
          std::pair{"proxy.username", ""},
          std::pair{"auth.endpoint", "http://login.example.test"},
          std::pair{"graph.endpoint", "https://user:secret@graph.example.test"},
          std::pair{"auth.endpoint", "https://login.example.test?secret=x"}}) {
        auto* edit = window.findChild<QLineEdit*>(field);
        const auto valid = edit->text();
        edit->setText(invalid);
        require(
            !save(window).startsWith("Settings saved.") &&
                test::read_file(file) == saved && backups(fixture.path()) == 1,
            "invalid phase two input wrote configuration or backup"
        );
        edit->setText(valid);
    }
    for (const auto* invalid :
         {"localhost\n", "local host", "host,other", "\nhost"}) {
        list(window, "proxy.no_proxy", invalid);
        require(
            !save(window).startsWith("Settings saved.") &&
                test::read_file(file) == saved && backups(fixture.path()) == 1,
            "invalid bypass list wrote configuration"
        );
    }
    window.findChild<QPushButton*>("reloadSettingsButton")->click();
    require(
        editor->patches().empty() &&
            window.findChild<QPlainTextEdit*>("proxy.no_proxy")
                ->toPlainText()
                .contains("127.0.0.0/8"),
        "global reload did not restore phase two fields"
    );
    for (const auto* name :
         {"proxy.password_file",
          "proxy.ca_file",
          "proxy.username",
          "logging.file",
          "sync.data_mount_point"}) {
        text(window, name, "");
    }
    choice(window, "proxy.auth", "auto");
    checked(window, "proxy.url.override", false);
    checked(window, "proxy.no_proxy.override", false);
    require(
        save(window).startsWith("Settings saved."), "optional unset failed"
    );
    const auto unset = config::Config::load(file);
    require(
        !unset.proxy.url && !unset.proxy.no_proxy && !unset.proxy.username &&
            !unset.proxy.password_file && !unset.proxy.ca_file &&
            !unset.logging.file && !unset.sync_data_mount_point &&
            !test::read_file(file).contains("[proxy]") &&
            test::read_file(file).contains("# keep proxy") &&
            test::read_file(file).contains("# keep bypass") &&
            test::read_file(file).contains("# keep file"),
        "optional unset retained proxy table or discarded comments"
    );
}

void test_proxy_override_and_nested_shapes() {
    gui::SettingsEditor editor;
    for (const auto* proxy :
         {"",
          "[proxy] # empty proxy table\n",
          "[proxy]\nno_proxy = []\n",
          "[proxy]\nno_proxy = ['localhost']\n",
          "[proxy]\nurl = ''\n",
          "[proxy]\nurl = ''\nno_proxy = []\n",
          "[proxy]\nurl = 'socks5h://localhost:1080'\nno_proxy = "
          "['.example.test', '::1']\n"}) {
        const auto source = "config_version = 2\n" + std::string{proxy};
        const auto c = config::Config::load_from_string("config.toml", source);
        editor.load(c);
        require(
            editor.findChild<QCheckBox*>("proxy.url.override")->isChecked() ==
                    c.proxy.url.has_value() &&
                editor.findChild<QCheckBox*>("proxy.no_proxy.override")
                        ->isChecked() == c.proxy.no_proxy.has_value() &&
                editor.patches().empty(),
            "reload collapsed absent/empty proxy states"
        );
        checked(editor, "proxy.url.override", true);
        text(editor, "proxy.url", "");
        checked(editor, "proxy.no_proxy.override", true);
        list(editor, "proxy.no_proxy", "");
        auto updated =
            config::update_settings("config.toml", source, editor.patches());
        const auto empty =
            config::Config::load_from_string("config.toml", updated);
        require(
            empty.proxy.url == "" &&
                empty.proxy.no_proxy == std::vector<std::string>{},
            "explicit empty overrides failed"
        );
        editor.load(empty);
        checked(editor, "proxy.no_proxy.override", false);
        updated =
            config::update_settings("config.toml", updated, editor.patches());
        require(
            !config::Config::load_from_string("config.toml", updated)
                 .proxy.no_proxy,
            "bypass unset became an empty override"
        );
        editor.load(config::Config::load_from_string("config.toml", updated));
        checked(editor, "proxy.url.override", false);
        updated =
            config::update_settings("config.toml", updated, editor.patches());
        require(
            !config::Config::load_from_string("config.toml", updated).proxy.url,
            "proxy unset failed to restore inheritance"
        );
    }
    for (const auto* shape :
         {"[graph.throttle] # keep nested\nmaximum_retries = 4\n",
          "graph = { throttle = { maximum_retries = 4 } } # keep nested\n",
          "graph = { endpoint = 'https://graph.microsoft.com/v1.0' } # keep "
          "nested\n",
          "graph.throttle.maximum_retries = 4 # keep nested\n",
          "[\"graph\".\"throttle\"] # keep nested\nmaximum_retries = 4\n"}) {
        const auto updated = config::update_settings(
            "config.toml",
            "config_version = 2\n" + std::string{shape},
            {{"graph.throttle", "maximum_retries", std::int64_t{9}},
             {"graph.throttle", "initial_delay_seconds", std::int64_t{3}},
             {"graph.throttle", "maximum_delay_seconds", std::int64_t{7}},
             {"auth",
              "scopes",
              std::vector<std::string>{"User.Read", "Files.ReadWrite"}},
             {"graph", "endpoint", std::string{"https://graph.example.test"}}}
        );
        const auto c = config::Config::load_from_string("config.toml", updated);
        require(
            c.graph_maximum_throttle_retries == 9 &&
                c.graph_initial_throttle_delay.count() == 3 &&
                c.graph_maximum_throttle_delay.count() == 7 &&
                c.auth_scope == "User.Read Files.ReadWrite" &&
                c.graph_endpoint == "https://graph.example.test" &&
                updated.contains("# keep nested"),
            "nested/inline/dotted typed patch failed"
        );
    }
    for (const auto* shape :
         {"[proxy] # keep proxy\nurl = '' # keep URL\nno_proxy = [] # keep "
          "list\n",
          "proxy = { url = '', no_proxy = [] } # keep inline\n",
          "proxy.url = '' # keep dotted\nproxy.no_proxy = [] # keep list\n"}) {
        const auto updated = config::update_settings(
            "config.toml",
            "config_version = 2\n" + std::string{shape},
            {{"proxy", "", std::monostate{}}}
        );
        require(
            !config::Config::load_from_string("config.toml", updated)
                    .proxy.url &&
                updated.contains("# keep"),
            "proxy removal broke shape or comments"
        );
    }
}
void test_phase_two_enums_and_lower_limits() {
    gui::Localization localization{gui::Language::english};
    gui::SettingsEditor editor;
    const std::string source =
        "config_version = 2\n[proxy]\nurl = 'https://proxy.example.test'\n";
    editor.load(config::Config::load_from_string("config.toml", source));
    for (const auto* name :
         {"proxy.auth",
          "transfer.http_version",
          "transfer.ip_version",
          "logging.level",
          "sync.permissions",
          "filesystem.metadata"}) {
        auto* combo = editor.findChild<QComboBox*>(name);
        for (int index = 0; index < combo->count(); ++index) {
            editor.load(
                config::Config::load_from_string("config.toml", source)
            );
            combo->setCurrentIndex(index);
            const auto data = combo->currentData();
            const auto patches = editor.patches();
            localization.set_language(gui::Language::simplified_chinese);
            editor.retranslate();
            require(
                combo->currentData() == data &&
                    same_patches(patches, editor.patches()),
                "new enum changed parser data across language switch"
            );
            const auto updated = config::update_settings(
                "config.toml", source, editor.patches()
            );
            editor.load(
                config::Config::load_from_string("config.toml", updated)
            );
            require(
                combo->currentData() == data && editor.patches().empty(),
                "new enum failed actual Config reload"
            );
            localization.set_language(gui::Language::english);
            editor.retranslate();
        }
    }
    editor.load(config::Config::defaults());
    for (const auto& [name, number] :
         {std::pair{"transfer.connect_timeout_seconds", "1"},
          std::pair{"transfer.operation_timeout_seconds", "1"},
          std::pair{"transfer.stall_timeout_seconds", "0"},
          std::pair{"transfer.stall_minimum_bytes_per_second", "1"},
          std::pair{"graph.throttle.maximum_retries", "0"},
          std::pair{"graph.throttle.initial_delay_seconds", "0"},
          std::pair{"graph.throttle.maximum_delay_seconds", "0"},
          std::pair{"monitor.poll_interval_seconds", "1"},
          std::pair{"monitor.settle_delay_milliseconds", "0"},
          std::pair{"monitor.websocket_request_timeout_seconds", "1"},
          std::pair{"monitor.websocket_connect_timeout_seconds", "1"},
          std::pair{"monitor.websocket_renewal_lead_seconds", "0"},
          std::pair{"monitor.websocket_initial_backoff_seconds", "1"},
          std::pair{"monitor.websocket_maximum_backoff_seconds", "1"}}) {
        text(editor, name, number);
    }
    const auto updated = config::update_settings(
        "config.toml", "config_version = 2\n", editor.patches()
    );
    const auto c = config::Config::load_from_string("config.toml", updated);
    require(
        c.transfer_transport.connect_timeout.count() == 1 &&
            c.transfer_transport.operation_timeout.count() == 1 &&
            c.transfer_transport.low_speed_timeout.count() == 0 &&
            c.transfer_transport.low_speed_limit_bytes_per_second == 1 &&
            c.graph_maximum_throttle_retries == 0 &&
            c.graph_initial_throttle_delay.count() == 0 &&
            c.graph_maximum_throttle_delay.count() == 0 &&
            c.monitor_poll_interval.count() == 1 &&
            c.monitor_settle_delay.count() == 0 &&
            c.monitor_websocket_request_timeout.count() == 1 &&
            c.monitor_websocket_connect_timeout.count() == 1 &&
            c.monitor_websocket_renewal_lead.count() == 0 &&
            c.monitor_websocket_initial_backoff.count() == 1 &&
            c.monitor_websocket_maximum_backoff.count() == 1,
        "phase two lower boundaries failed"
    );
    editor.load(c);
    require(editor.patches().empty(), "lower limits generated edits on reload");
}

void test_array_comment_preservation() {
    for (const auto* shape :
         {"[proxy]\nurl = ''\nno_proxy = [ # keep before\n"
          "'.old.test', # keep first\n'::1' # keep last\n] # keep after\n",
          "proxy = { url = '', no_proxy = [ # keep before\n"
          "'.old.test', # keep first\n'::1' # keep last\n] } # keep after\n"}) {
        const auto original = "config_version = 2\n" + std::string{shape};
        for (const auto& patch :
             {config::SettingPatch{
                  "proxy", "no_proxy", std::vector<std::string>{"new.test"}
              },
              config::SettingPatch{"proxy", "no_proxy", std::monostate{}},
              config::SettingPatch{"proxy", "", std::monostate{}}}) {
            const auto updated =
                config::update_settings("config.toml", original, {patch});
            for (const auto* comment :
                 {"# keep before",
                  "# keep first",
                  "# keep last",
                  "# keep after"}) {
                require(
                    updated.contains(comment),
                    "array patch/removal discarded an internal comment"
                );
            }
            const auto c =
                config::Config::load_from_string("config.toml", updated);
            require(
                c.proxy.no_proxy ==
                    (std::holds_alternative<std::vector<std::string>>(
                         patch.value
                     )
                         ? std::optional<std::vector<std::string>>{{"new.test"}}
                         : std::nullopt),
                "array comment retention changed the saved value"
            );
        }
    }
    const auto scopes = config::update_settings(
        "config.toml",
        "config_version = 2\n[auth]\nscopes = ['User.Read', # keep scope\n"
        "'Literal#NotAComment']\n",
        {{"auth",
          "scopes",
          std::vector<std::string>{"User.Read", "offline_access"}}}
    );
    require(
        scopes.contains("# keep scope") &&
            !scopes.contains("Literal#NotAComment") &&
            config::Config::load_from_string("config.toml", scopes)
                    .auth_scope == "User.Read offline_access",
        "array comment scanner treated a hash inside a string as a comment"
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
            std::none_of(patches.begin(), patches.end(), [](const auto& patch) {
                return patch.table == "proxy" && patch.key.empty();
            }),
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
        backup && test::read_file(*backup) == original &&
            c.proxy.url == "" &&
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

void test_phase_two_patch_boundaries() {
    const std::string source = "config_version = 2\n";
    require(
        config::update_settings(
            "config.toml", source, {{"proxy", "", std::monostate{}}}
        ) == source,
        "removing an absent proxy table changed the source"
    );
    for (const auto& patch :
         {config::SettingPatch{"proxy", "", std::string{"invalid"}},
          config::SettingPatch{"graph.throttle", "", std::monostate{}},
          config::SettingPatch{"graph", "", std::monostate{}}}) {
        require(
            test::throws_with<std::invalid_argument>([&] {
                static_cast<void>(config::update_settings(
                    "config.toml", source + "[graph.throttle]\n", {patch}
                ));
            }),
            "unsafe table removal was accepted"
        );
    }
    const auto nested = config::update_settings(
        "config.toml", source + "graph = {} # keep empty parent\n",
        {{"graph.throttle", "maximum_retries", std::int64_t{0}}}
    );
    require(
        nested.contains("# keep empty parent") &&
            config::Config::load_from_string("config.toml", nested)
                    .graph_maximum_throttle_retries == 0,
        "inserting throttle into an empty inline parent failed"
    );
    gui::SettingsEditor editor;
    editor.load(config::Config::defaults());
    for (const auto* field :
         {"transfer.connect_timeout_seconds",
          "transfer.operation_timeout_seconds",
          "transfer.stall_timeout_seconds",
          "graph.throttle.maximum_retries",
          "graph.throttle.initial_delay_seconds",
          "graph.throttle.maximum_delay_seconds"}) {
        text(editor, field, "9223372036854775807");
    }
    const auto c = config::Config::load_from_string(
        "config.toml",
        config::update_settings("config.toml", source, editor.patches())
    );
    require(
        c.transfer_transport.connect_timeout.count() == INT64_MAX &&
            c.transfer_transport.operation_timeout.count() == INT64_MAX &&
            c.transfer_transport.low_speed_timeout.count() == INT64_MAX &&
            c.graph_maximum_throttle_retries == INT64_MAX &&
            c.graph_initial_throttle_delay.count() == INT64_MAX &&
            c.graph_maximum_throttle_delay.count() == INT64_MAX,
        "timeout/throttle upper TOML int64 boundary was truncated"
    );
    editor.load(c);
    require(editor.patches().empty(), "upper limits generated reload patches");
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "not-created" / "config.toml";
    for (const auto& [sync, state] :
         {std::pair{"", "state"}, std::pair{"sync", ""}}) {
        require(
            test::throws_with<std::invalid_argument>([&] {
                static_cast<void>(gui::save_basic_settings(file, sync, state));
            }) &&
                !std::filesystem::exists(file.parent_path()),
            "empty basic settings created directories or configuration"
        );
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    try {
        test_tooltips_and_language_switching();
        test_tooltips_initial_locale_and_reload();
        test_tooltips_expanded_groups_and_disabled_drafts();
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
        test_phase_two_round_trip();
        test_proxy_override_and_nested_shapes();
        test_phase_two_enums_and_lower_limits();
        test_array_comment_preservation();
        test_endpoint_import_rejection();
        test_phase_two_proxy_drafts_and_empty_save();
        test_phase_two_patch_boundaries();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
