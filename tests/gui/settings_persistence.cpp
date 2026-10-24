#include "settings_support.hpp"
#include "settings_cases.hpp"

namespace onedrive::test::gui_settings {

void test_round_trip() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto sync = fixture.path() / "sync";
    const auto state = fixture.path() / "state";
    const auto original = std::string{
        "# 中文 comment before\nconfig_version = 2\n"
        "[auth] # auth header\napplication_id = 'old-client' # retain auth\n"
        "[sync] # rules header\nmode = 'bidirectional' # retain mode\n"
        "drive_id = 'legacy-drive' # retain drive comment\n"
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
    require(
        window.findChild<gui::SettingsEditor*>()->patches().empty(),
        "loading a legacy Drive ID created a migration edit"
    );
    auto* tabs = window.findChild<QTabWidget*>("settingsTabs");
    require(tabs && tabs->count() == 6, "six settings tabs are missing");
    text(window, "sync.data_directory", QString::fromStdString(sync.string()));
    text(window, "state.directory", QString::fromStdString(state.string()));
    text(window, "auth.application_id", "client-中文\"\\");
    text(window, "auth.tenant_id", "organizations");
    list(window, "sync.drive_ids", "drive-中文\nsecond-drive");
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
        drafts.size() == 29,
        "all fields and sync-mode/Drive compatibility patches expected"
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
            config.drive_ids ==
                std::vector<std::string>{"drive-中文", "second-drive"} &&
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
            saved.contains("# retain drive comment") &&
            saved.contains("# retain concurrency") &&
            saved.contains("[console]\ntheme = 'hacker' # CLI only\n") &&
            saved.contains("[monitor]\npoll_interval_seconds = 432\n") &&
            saved.contains(
                "[graph]\nendpoint = 'https://graph.microsoft.com/v1.0'\n"
            ),
        "save changed comments or unrelated configuration"
    );
    require(backups(fixture.path()) == 1, "save did not create one backup");
    for (const auto* invalid :
         {"",
          "first\nfirst",
          "first\n",
          "first\nsecond drive",
          "first\nhttps://contoso.sharepoint.com/sites/team",
          "first\n//contoso.sharepoint.com/sites/team"}) {
        list(window, "sync.drive_ids", invalid);
        require(
            !save(window).startsWith("Settings saved.") &&
                test::read_file(file) == saved && backups(fixture.path()) == 1,
            "invalid Drive list changed configuration or created a backup"
        );
    }
    list(window, "sync.drive_ids", "drive-中文\nsecond-drive");
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
                {"sync", "drive_id", std::string{"新-drive"}},
                {"sync", "maximum_file_size_bytes", std::int64_t{4294967297}},
                {"sync", "sync_list", std::monostate{}},
                {"upload", "concurrency", std::int64_t{2}},
                {"upload", "maximum_rate_bytes_per_second", std::int64_t{42}},
            }
        );
        const auto loaded =
            config::Config::load_from_string("config.toml", updated);
        require(
            loaded.drive_id == "新-drive" &&
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

} // namespace onedrive::test::gui_settings
