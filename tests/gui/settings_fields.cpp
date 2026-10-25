#include "settings_support.hpp"
#include "settings_cases.hpp"

namespace onedrive::test::gui_settings {

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

    tabs->setCurrentIndex(0);
    text(editor, "auth.application_id", "sp-client-id");
    auto* drive_ids = editor.findChild<QPlainTextEdit*>("sync.drive_ids");
    auto* browse_sp = editor.findChild<QPushButton*>("browseSharePointButton");
    require(
        drive_ids != nullptr && browse_sp != nullptr,
        "SharePoint library browser button missing"
    );
    std::vector<std::string> queries;
    std::string observed_app_id;
    editor.set_site_search_function(
        [&](const config::Config& cfg,
            const std::string& query) -> std::vector<graph::SiteInfo> {
            observed_app_id = cfg.application_id;
            queries.push_back(query);
            if (query == "error") {
                throw std::runtime_error{"simulated 403 Sites.Read.All"};
            }
            if (query == "missing") {
                return {};
            }
            return {
                graph::SiteInfo{
                    .id = "contoso.sharepoint.com,site-1,web-1",
                    .name = "engineering",
                    .display_name = "Engineering",
                    .web_url = "https://contoso.sharepoint.com/sites/eng",
                    .drives = {
                        graph::DriveInfo{
                            .id = "me",
                            .name = "Existing Default",
                            .type = "documentLibrary",
                            .web_url = "https://contoso.sharepoint.com/sites/"
                                       "eng/Default",
                        },
                        graph::DriveInfo{
                            .id = "drv-eng-docs",
                            .name = "Documents",
                            .type = "documentLibrary",
                            .web_url = "https://contoso.sharepoint.com/sites/"
                                       "eng/Shared%20Documents",
                        },
                        graph::DriveInfo{
                            .id = "drv-eng-specs",
                            .name = "Specs",
                            .type = "documentLibrary",
                            .web_url = "https://contoso.sharepoint.com/sites/"
                                       "eng/Specs",
                        },
                    },
                },
            };
        }
    );
    bool sp_handled = false;
    QTimer sp_dialogs;
    QObject::connect(&sp_dialogs, &QTimer::timeout, &editor, [&] {
        auto* dialog =
            qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "sharePointBrowserDialog" ||
            sp_handled) {
            return;
        }
        sp_handled = true;
        auto* search_edit =
            dialog->findChild<QLineEdit*>("sharePointSearchEdit");
        auto* search_btn =
            dialog->findChild<QPushButton*>("sharePointSearchButton");
        auto* status = dialog->findChild<QLabel*>("sharePointBrowserStatus");
        auto* tree = dialog->findChild<QTreeWidget*>("sharePointLibrariesTree");
        require(
            search_edit && search_btn && status && tree,
            "SharePoint browser dialog controls missing"
        );
        search_edit->setText("error");
        search_btn->click();
        require(
            status->text().contains("simulated 403 Sites.Read.All"),
            "SharePoint browser did not surface query failure"
        );
        search_edit->setText("missing");
        search_btn->click();
        require(
            tree->topLevelItemCount() == 1 &&
                tree->topLevelItem(0)->childCount() == 3 &&
                status->text().contains("missing"),
            "SharePoint browser did not fall back to wildcard site listing"
        );
        search_edit->clear();
        search_btn->click();
        require(
            tree->topLevelItemCount() == 1 &&
                tree->topLevelItem(0)->childCount() == 3,
            "SharePoint browser default wildcard search failed"
        );
        tree->topLevelItem(0)->child(0)->setCheckState(0, Qt::Checked);
        tree->topLevelItem(0)->child(1)->setCheckState(0, Qt::Checked);
        tree->topLevelItem(0)->child(2)->setCheckState(0, Qt::Checked);
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    sp_dialogs.start(1);
    browse_sp->click();
    sp_dialogs.stop();
    require(
        sp_handled && observed_app_id == "sp-client-id" &&
            queries == std::vector<std::string>{"error", "missing", "*", "*"} &&
            drive_ids->toPlainText() == "me\ndrv-eng-docs\ndrv-eng-specs",
        "SharePoint browser did not deduplicate and append selected Drive IDs"
    );

    // Also verify selecting a library row without checking any checkbox appends
    // that library's Drive ID.
    bool sp_row_selected = false;
    QObject::connect(&sp_dialogs, &QTimer::timeout, &editor, [&] {
        auto* dialog =
            qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "sharePointBrowserDialog" ||
            sp_row_selected) {
            return;
        }
        sp_row_selected = true;
        auto* search_btn =
            dialog->findChild<QPushButton*>("sharePointSearchButton");
        auto* tree = dialog->findChild<QTreeWidget*>("sharePointLibrariesTree");
        require(search_btn && tree, "SharePoint browser tree missing");
        search_btn->click();
        require(
            tree->topLevelItemCount() == 1 &&
                tree->topLevelItem(0)->childCount() == 3,
            "SharePoint browser did not populate items for row selection"
        );
        tree->setCurrentItem(tree->topLevelItem(0)->child(2));
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    drive_ids->setPlainText("me");
    sp_dialogs.start(1);
    browse_sp->click();
    sp_dialogs.stop();
    require(
        sp_row_selected && drive_ids->toPlainText() == "me\ndrv-eng-specs",
        "SharePoint browser did not fall back to selected tree row Drive ID"
    );
}

void test_configuration_fingerprint() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    bool missing = false;
    std::string fingerprint = "previous";
    static_cast<void>(gui::load_configuration(file, missing, &fingerprint));
    require(
        missing && fingerprint.empty(),
        "missing configuration retained an old source fingerprint"
    );
    const std::string source =
        "config_version = 2\n[sync]\ndata_directory = 'sync'\n"
        "[state]\ndirectory = 'state'\n";
    test::write_file(file, source);
    const auto loaded = gui::load_configuration(file, missing, &fingerprint);
    require(
        !missing && fingerprint == util::sha256_hex(source) &&
            loaded.sync_data_directory == std::filesystem::path{"sync"},
        "configuration fingerprint did not match parsed source bytes"
    );
    const auto valid_fingerprint = fingerprint;
    test::write_file(file, "config_version = 2\n[sync\n");
    require(
        test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    gui::load_configuration(file, missing, &fingerprint)
                );
            },
            "cannot parse TOML"
        ) && fingerprint == valid_fingerprint,
        "invalid configuration published a new fingerprint or hid its error"
    );
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
        "config.toml",
        source + "graph = {} # keep empty parent\n",
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

} // namespace onedrive::test::gui_settings
