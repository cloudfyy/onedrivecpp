#include "settings_support.hpp"
#include "settings_cases.hpp"

namespace onedrive::test::gui_settings {

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
    text(window, "history.maximum_entries", "7");
    choice(window, "sync.permissions", "umask");
    choice(window, "filesystem.metadata", "database");
    const auto draft = editor->patches();
    require(draft.size() == 32, "phase two does not patch every new field");
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
            c.file_history_limit == 7 &&
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

} // namespace onedrive::test::gui_settings
