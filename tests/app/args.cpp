#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "ui/cli/app/args.hpp"

#include <array>
#include <utility>

namespace {

namespace exit_code = onedrive::exit_code;

static_assert(exit_code::success == 0);
static_assert(exit_code::failure == 1);
static_assert(exit_code::usage_error == 2);
static_assert(exit_code::cancelled == 130);
static_assert(exit_code::blocked == 3);
static_assert(exit_code::blocked != exit_code::usage_error);

using namespace onedrive::test::app;

onedrive::app::detail::ParseResult
parse_test_arguments(std::vector<std::string> arguments) {
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }
    return onedrive::app::detail::parse_arguments(
        static_cast<int>(pointers.size()), pointers.data()
    );
}

int test_ui_option_help() {
    FakeRuntimeFactory runtime_factory;
    for (const auto& [group, action] : {
             std::pair{"account", "login"},
             {"transfer", "sync"},
             {"transfer", "download"},
             {"transfer", "restore"},
             {"transfer", "watch"},
             {"inspect", "health"},
             {"inspect", "status"},
             {"inspect", "blocked"},
             {"inspect", "drives"},
             {"inspect", "shared"},
             {"inspect", "sites"},
             {"inspect", "quota"},
             {"inspect", "storage"},
             {"inspect", "partials"},
             {"inspect", "files"},
             {"inspect", "verify"},
             {"inspect", "config"},
         }) {
        const auto help = run_application(
            runtime_factory, {"onedrive-cpp", group, action, "--help"}
        );
        if (help.exit_code != exit_code::success ||
            !help.standard_output.contains("--ui {auto,console,tui} [auto]") ||
            !help.standard_output.contains(
                "--theme {hacker,ocean,amber,synthwave} [hacker]"
            )) {
            return fail(
                std::string{group} + " " + action +
                " help did not show readable UI/theme choices\n" +
                help.standard_output
            );
        }
    }
    using onedrive::cli::TuiTheme;
    using onedrive::cli::UiMode;
    constexpr std::array modes{
        std::pair{"auto", UiMode::automatic},
        std::pair{"console", UiMode::console},
        std::pair{"tui", UiMode::tui},
    };
    constexpr std::array themes{
        std::pair{"hacker", TuiTheme::hacker},
        std::pair{"ocean", TuiTheme::ocean},
        std::pair{"amber", TuiTheme::amber},
        std::pair{"synthwave", TuiTheme::synthwave},
    };
    for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        for (std::size_t theme = 0; theme < themes.size(); ++theme) {
            for (const bool numeric : {false, true}) {
                const auto result = parse_test_arguments({
                    "onedrive-cpp",
                    "inspect",
                    "config",
                    "--ui",
                    numeric ? std::to_string(mode) : modes[mode].first,
                    "--theme",
                    numeric ? std::to_string(theme) : themes[theme].first,
                });
                if (result.exit_code ||
                    result.arguments.ui_mode != modes[mode].second ||
                    result.arguments.tui_theme != themes[theme].second) {
                    return fail(
                        "readable help changed UI/theme argument mapping"
                    );
                }
            }
        }
    }
    const auto defaults =
        parse_test_arguments({"onedrive-cpp", "inspect", "config"});
    if (defaults.exit_code || defaults.arguments.ui_mode ||
        defaults.arguments.tui_theme) {
        return fail("UI/theme help defaults overrode configured preferences");
    }
    for (const auto* option : {"--ui", "--theme"}) {
        const auto result = run_application(
            runtime_factory,
            {"onedrive-cpp", "inspect", "config", option, "999"}
        );
        if (result.exit_code != exit_code::usage_error ||
            !result.standard_error.contains(option)) {
            return fail("readable help disabled numeric enum validation");
        }
    }
    return EXIT_SUCCESS;
}

int test_args() {
    FakeRuntimeFactory runtime_factory;
    for (const std::string scope : {"", "Documents", "Documents/report.pdf"}) {
        std::vector<std::string> arguments{
            "onedrive-cpp", "transfer", "restore", "--dry-run"
        };
        if (!scope.empty()) {
            arguments.push_back(scope);
        }
        const auto restored = parse_test_arguments(std::move(arguments));
        if (restored.exit_code ||
            restored.arguments.operation !=
                onedrive::app::detail::Operation::restore ||
            restored.arguments.restore_path != scope ||
            !restored.arguments.force_dry_run) {
            return fail(
                "restore did not accept its file, directory or Drive scope"
            );
        }
    }

    const auto help =
        run_application(runtime_factory, {"build/release/onedrive-cpp"});
    if (help.exit_code != exit_code::success ||
        !help.standard_output.contains("account") ||
        !help.standard_output.contains("inspect") ||
        !help.standard_output.contains("state") ||
        !help.standard_output.contains("transfer") ||
        !help.standard_output.contains("monitor") ||
        !help.standard_output.contains("onedrive-cpp [OPTIONS] SUBCOMMAND") ||
        help.standard_output.contains("build/release/onedrive-cpp")) {
        return fail("no-argument invocation did not show command help");
    }

    const auto version =
        run_application(runtime_factory, {"onedrive-cpp", "--version"});
    if (version.exit_code != exit_code::success ||
        !version.standard_output.starts_with("onedrive-cpp ")) {
        return fail("--version did not print the application version");
    }
    const auto help_banner = version.standard_output + "\n";
    if (!help.standard_output.starts_with(help_banner)) {
        return fail(
            "no-argument help omitted the application name and version"
        );
    }
    for (const auto& arguments : {
             std::vector<std::string>{"onedrive-cpp", "--help"},
             {"onedrive-cpp", "-h"},
             {"onedrive-cpp", "account", "--help"},
             {"onedrive-cpp", "account", "login", "--help"},
             {"onedrive-cpp", "inspect", "status", "--help"},
             {"onedrive-cpp", "state", "--help"},
             {"onedrive-cpp", "transfer", "sync", "--help"},
         }) {
        const auto command_help = run_application(runtime_factory, arguments);
        if (command_help.exit_code != exit_code::success ||
            !command_help.standard_output.starts_with(help_banner) ||
            !command_help.standard_error.empty()) {
            return fail(
                "command help omitted the application name and version"
            );
        }
    }
    const auto transfer_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "--help"}
    );
    using onedrive::app::detail::Operation;
    for (const auto& [action, operation] : {
             std::pair{"status", Operation::monitor_status},
             {"trigger", Operation::monitor_trigger},
             {"stop", Operation::monitor_stop},
         }) {
        const auto parsed = parse_test_arguments(
            {"onedrive-cpp", "monitor", action, "--output", "json", "--quiet"}
        );
        const auto control_help = run_application(
            runtime_factory, {"onedrive-cpp", "monitor", action, "--help"}
        );
        if (parsed.exit_code || parsed.arguments.operation != operation ||
            control_help.exit_code != exit_code::success ||
            !control_help.standard_output.contains("--config") ||
            !control_help.standard_output.contains("--output") ||
            control_help.standard_output.contains("--ui")) {
            return fail(
                "Monitor control parser/help did not preserve offline console "
                "options"
            );
        }
    }
    if (transfer_help.exit_code != exit_code::success ||
        !transfer_help.standard_output.contains("sync") ||
        !transfer_help.standard_output.contains("download") ||
        !transfer_help.standard_output.contains("watch")) {
        return fail("transfer help did not list its transfer actions");
    }
    const auto sync_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "sync", "--help"}
    );
    if (sync_help.exit_code != exit_code::success ||
        !sync_help.standard_output.contains("--force-large-delete") ||
        !sync_help.standard_output.contains("--ui") ||
        !sync_help.standard_output.contains("--theme")) {
        return fail("sync help did not document sync-specific options");
    }
    const auto account_help =
        run_application(runtime_factory, {"onedrive-cpp", "account", "--help"});
    if (account_help.exit_code != exit_code::success ||
        !account_help.standard_output.contains("login") ||
        !account_help.standard_output.contains("logout")) {
        return fail("account help did not list its authentication actions");
    }
    const auto login_help = run_application(
        runtime_factory, {"onedrive-cpp", "account", "login", "--help"}
    );
    if (login_help.exit_code != exit_code::success ||
        !login_help.standard_output.contains("--ui") ||
        !login_help.standard_output.contains("--theme")) {
        return fail("account login help did not document TUI options");
    }
    const auto watch_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "watch", "--help"}
    );
    if (watch_help.exit_code != exit_code::success ||
        !watch_help.standard_output.contains("--ui") ||
        !watch_help.standard_output.contains("--theme")) {
        return fail("transfer watch help did not document TUI options");
    }
    const auto download_help = run_application(
        runtime_factory,
        {"onedrive-cpp", "transfer", "download", "--help"}
    );
    if (download_help.exit_code != exit_code::success ||
        !download_help.standard_output.contains("--ui") ||
        !download_help.standard_output.contains("--theme")) {
        return fail("download help did not document TUI options");
    }
    const auto inspect_help =
        run_application(runtime_factory, {"onedrive-cpp", "inspect", "--help"});
    if (inspect_help.exit_code != exit_code::success ||
        !inspect_help.standard_output.contains("health") ||
        !inspect_help.standard_output.contains("status") ||
        !inspect_help.standard_output.contains("blocked") ||
        !inspect_help.standard_output.contains("drives") ||
        !inspect_help.standard_output.contains("shared") ||
        !inspect_help.standard_output.contains("sites") ||
        !inspect_help.standard_output.contains("quota") ||
        !inspect_help.standard_output.contains("storage") ||
        !inspect_help.standard_output.contains("partials") ||
        !inspect_help.standard_output.contains("files") ||
        !inspect_help.standard_output.contains("verify") ||
        !inspect_help.standard_output.contains("config")) {
        return fail("inspect help did not list its read-only actions");
    }
    const auto health_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "health", "--help"}
    );
    if (health_help.exit_code != exit_code::success ||
        !health_help.standard_output.contains(
            "Run local synchronization-state diagnostics"
        ) ||
        !health_help.standard_output.contains("--ui") ||
        !health_help.standard_output.contains("--theme")) {
        return fail("inspect health help was not available");
    }
    const auto status_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "status", "--help"}
    );
    if (status_help.exit_code != exit_code::success ||
        !status_help.standard_output.contains(
            "Show read-only synchronization status"
        ) ||
        !status_help.standard_output.contains("--ui") ||
        !status_help.standard_output.contains("--theme")) {
        return fail("status help was not available");
    }
    const auto blocked_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "blocked", "--help"}
    );
    if (blocked_help.exit_code != exit_code::success ||
        !blocked_help.standard_output.contains("--reason") ||
        !blocked_help.standard_output.contains("--search") ||
        !blocked_help.standard_output.contains("--output") ||
        !blocked_help.standard_output.contains("--ui")) {
        return fail("inspect blocked help was not available");
    }
    const auto blocked = parse_test_arguments(
        {"onedrive-cpp",
         "inspect",
         "blocked",
         "--reason",
         "future_reason",
         "--search",
         "Documents"}
    );
    if (blocked.exit_code ||
        blocked.arguments.operation !=
            onedrive::app::detail::Operation::blocked ||
        blocked.arguments.blocked_reason != "future_reason" ||
        blocked.arguments.blocked_search != "Documents") {
        return fail("inspect blocked arguments were not preserved");
    }
    const auto files_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "files", "--help"}
    );
    if (files_help.exit_code != exit_code::success ||
        !files_help.standard_output.contains("[PATH]") ||
        !files_help.standard_output.contains("--status") ||
        !files_help.standard_output.contains("--ui")) {
        return fail("inspect files help was not available");
    }
    const auto verify_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "verify", "--help"}
    );
    if (verify_help.exit_code != exit_code::success ||
        !verify_help.standard_output.contains("--mode") ||
        !verify_help.standard_output.contains("[PATH]")) {
        return fail("inspect verify help was not available");
    }
    if (run_application(
            runtime_factory,
            {
                "onedrive-cpp",
                "inspect",
                "files",
                "--status",
                "corrupt",
            }
        )
            .exit_code != exit_code::usage_error) {
        return fail("inspect files accepted an invalid status");
    }
    const auto state_help =
        run_application(runtime_factory, {"onedrive-cpp", "state", "--help"});
    if (state_help.exit_code != exit_code::success ||
        !state_help.standard_output.contains("reset-cursor") ||
        !state_help.standard_output.contains("cleanup") ||
        !state_help.standard_output.contains("migrate") ||
        !state_help.standard_output.contains("clear")) {
        return fail("state help did not list its maintenance actions");
    }
    const auto cleanup_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "cleanup", "--help"}
    );
    const auto migrate_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "migrate", "--help"}
    );
    if (migrate_help.exit_code != exit_code::success ||
        !migrate_help.standard_output.contains("--dry-run") ||
        !migrate_help.standard_output.contains("--yes")) {
        return fail("state migrate help did not document safety options");
    }
    if (cleanup_help.exit_code != exit_code::success ||
        !cleanup_help.standard_output.contains("--dry-run") ||
        !cleanup_help.standard_output.contains("--yes")) {
        return fail("state cleanup help did not document safety options");
    }
    const auto reset_cursor_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "reset-cursor", "--help"}
    );
    if (reset_cursor_help.exit_code != exit_code::success ||
        reset_cursor_help.standard_output.contains("--yes")) {
        return fail("state reset-cursor help exposed clear-only options");
    }
    const auto clear_state_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "clear", "--help"}
    );
    if (clear_state_help.exit_code != exit_code::success ||
        !clear_state_help.standard_output.contains("--yes") ||
        clear_state_help.standard_output.contains("--clear-all")) {
        return fail("state clear help did not document its confirmation");
    }

    if (run_application(runtime_factory, {"onedrive-cpp", "unknown"})
            .exit_code != exit_code::usage_error) {
        return fail("unknown command did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "login", "--config"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("missing option value did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "login", "--dry-run"}
        )
            .exit_code != exit_code::usage_error) {
        return fail(
            "command-specific option was accepted by the wrong command"
        );
    }
    if (run_application(
            runtime_factory,
            {
                "onedrive-cpp",
                "transfer",
                "watch",
                "--force-large-delete",
            }
        )
            .exit_code != exit_code::usage_error) {
        return fail(
            "transfer watch accepted the one-shot large-delete override"
        );
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--log-level", "verbose"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("invalid log level did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--color", "sometimes"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("invalid color mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--output", "yaml"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("invalid output mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "transfer", "sync", "--ui", "graphical"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("invalid UI mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "transfer", "sync", "--theme", "rainbow"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("invalid TUI theme did not return usage exit code 2");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "state"}).exit_code !=
        exit_code::usage_error) {
        return fail("state command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "account"})
            .exit_code != exit_code::usage_error) {
        return fail("account command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "inspect"})
            .exit_code != exit_code::usage_error) {
        return fail("inspect command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "auth"}).exit_code !=
            exit_code::usage_error ||
        run_application(runtime_factory, {"onedrive-cpp", "logout"})
                .exit_code != exit_code::usage_error) {
        return fail("removed authentication commands were still accepted");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "state", "reset-cursor", "--yes"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("state reset-cursor accepted the clear confirmation flag");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "reset-state"})
            .exit_code != exit_code::usage_error) {
        return fail("removed reset-state command was still accepted");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "transfer"})
            .exit_code != exit_code::usage_error) {
        return fail("transfer command accepted a missing action");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "transfer", "download"}
        )
            .exit_code != exit_code::usage_error) {
        return fail("transfer download accepted a missing remote path");
    }
    const auto wildcard_sites =
        parse_test_arguments({"onedrive-cpp", "inspect", "sites"});
    if (wildcard_sites.exit_code ||
        wildcard_sites.arguments.operation !=
            onedrive::app::detail::Operation::sites ||
        !wildcard_sites.arguments.site_query.empty()) {
        return fail("inspect sites rejected an omitted default wildcard query");
    }
    for (const std::string_view command :
         {"doctor", "status", "drives", "shared", "sites", "quota"}) {
        if (run_application(
                runtime_factory, {"onedrive-cpp", std::string{command}}
            )
                .exit_code != exit_code::usage_error) {
            return fail("removed inspection command was still accepted");
        }
    }
    for (const std::string_view command : {"sync", "download", "monitor"}) {
        if (run_application(
                runtime_factory, {"onedrive-cpp", std::string{command}}
            )
                .exit_code != exit_code::usage_error) {
            return fail("removed transfer command was still accepted");
        }
    }

    CliFixture ui_fixture;
    if (ui_fixture.authenticate().exit_code != exit_code::success) {
        return fail("UI precedence fixture could not authenticate");
    }
    auto configured = onedrive::test::read_file(ui_fixture.config_path);
    const auto color = configured.find("color = \"always\"\n");
    if (color == std::string::npos) {
        return fail("UI precedence fixture console section was missing");
    }
    configured.insert(
        color + std::string{"color = \"always\"\n"}.size(), "ui = \"tui\"\n"
    );
    onedrive::test::write_file(ui_fixture.config_path, configured);
    const auto configured_tui = run_application(
        ui_fixture.runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            ui_fixture.config_path.string(),
            "--dry-run",
        }
    );
    const auto overridden_tui = run_application(
        ui_fixture.runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            ui_fixture.config_path.string(),
            "--dry-run",
            "--ui",
            "console",
        }
    );
    const auto download_tui = run_application(
        ui_fixture.runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "download",
            "Documents/file.txt",
            "--config",
            ui_fixture.config_path.string(),
            "--dry-run",
        }
    );
    if (configured_tui.exit_code != exit_code::failure ||
        overridden_tui.exit_code != exit_code::success ||
        download_tui.exit_code != exit_code::failure) {
        return fail("CLI UI mode did not override configured TUI mode");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{2};
    if (const auto result = progress.run("ui_option_help", test_ui_option_help);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run("args", test_args));
}
