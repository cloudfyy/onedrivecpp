#include "settings_support.hpp"
#include "settings_cases.hpp"

namespace onedrive::test::gui_settings {

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
        for (const auto* btn_name :
             {"browseSharePointButton",
              "addDriveIdButton",
              "removeDriveIdButton",
              "addListItemButton",
              "removeListItemButton"}) {
            if (auto* btn = input->parentWidget()->findChild<QPushButton*>(
                    btn_name, Qt::FindDirectChildrenOnly
                )) {
                remember(btn, tip);
                remember(input->parentWidget(), tip);
            }
        }
        if (auto* override = editor->findChild<QCheckBox*>(
                input->objectName() + ".override"
            )) {
            remember(override, tip);
            remember(input->parentWidget(), tip);
        }
    }
    require(inputs == 58, "not all 58 settings inputs have tips");
    std::size_t controls = 0;
    for (auto* widget : editor->findChildren<QWidget*>()) {
        if (widget->objectName().contains('.') &&
            (qobject_cast<QLineEdit*>(widget) ||
             qobject_cast<QPlainTextEdit*>(widget) ||
             qobject_cast<QListWidget*>(widget) ||
             qobject_cast<QComboBox*>(widget) ||
             qobject_cast<QCheckBox*>(widget))) {
            ++controls;
            require(!widget->toolTip().isEmpty(), "uncovered settings control");
        }
    }
    require(controls == 60, "settings inputs or override controls missed");
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
        "history.maximum_entries",
        {"10000", "greater than 0", "Oldest", "not saved after exit"}
    );
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
            if (english_tips.size() < 58 && !chinese) {
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
            english_tips.size() == 58, "initial tooltip inventory incomplete"
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
                list_text(*editor, "proxy.no_proxy") ==
                    "localhost\n.example.test",
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

} // namespace onedrive::test::gui_settings
