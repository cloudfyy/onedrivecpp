#include "support.hpp"

namespace {

using namespace onedrive::test::app;

int test_args() {
    FakeRuntimeFactory runtime_factory;

    const auto help =
        run_application(runtime_factory, {"build/release/onedrive-cpp"});
    if (help.exit_code != 0 || !help.standard_output.contains("account") ||
        !help.standard_output.contains("inspect") ||
        !help.standard_output.contains("state") ||
        !help.standard_output.contains("transfer") ||
        !help.standard_output.contains("onedrive-cpp [OPTIONS] SUBCOMMAND") ||
        help.standard_output.contains("build/release/onedrive-cpp")) {
        return fail("no-argument invocation did not show command help");
    }

    const auto version =
        run_application(runtime_factory, {"onedrive-cpp", "--version"});
    if (version.exit_code != 0 ||
        !version.standard_output.starts_with("onedrive-cpp ")) {
        return fail("--version did not print the application version");
    }
    const auto transfer_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "--help"}
    );
    if (transfer_help.exit_code != 0 ||
        !transfer_help.standard_output.contains("sync") ||
        !transfer_help.standard_output.contains("download") ||
        !transfer_help.standard_output.contains("watch")) {
        return fail("transfer help did not list its transfer actions");
    }
    const auto sync_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "sync", "--help"}
    );
    if (sync_help.exit_code != 0 ||
        !sync_help.standard_output.contains("--force-large-delete") ||
        !sync_help.standard_output.contains("--ui") ||
        !sync_help.standard_output.contains("--theme")) {
        return fail("sync help did not document sync-specific options");
    }
    const auto account_help =
        run_application(runtime_factory, {"onedrive-cpp", "account", "--help"});
    if (account_help.exit_code != 0 ||
        !account_help.standard_output.contains("login") ||
        !account_help.standard_output.contains("logout")) {
        return fail("account help did not list its authentication actions");
    }
    const auto login_help = run_application(
        runtime_factory, {"onedrive-cpp", "account", "login", "--help"}
    );
    if (login_help.exit_code != 0 ||
        !login_help.standard_output.contains("--ui") ||
        !login_help.standard_output.contains("--theme")) {
        return fail("account login help did not document TUI options");
    }
    const auto watch_help = run_application(
        runtime_factory, {"onedrive-cpp", "transfer", "watch", "--help"}
    );
    if (watch_help.exit_code != 0 ||
        !watch_help.standard_output.contains("--ui") ||
        !watch_help.standard_output.contains("--theme")) {
        return fail("transfer watch help did not document TUI options");
    }
    const auto download_help = run_application(
        runtime_factory,
        {"onedrive-cpp", "transfer", "download", "--help"}
    );
    if (download_help.exit_code != 0 ||
        !download_help.standard_output.contains("--ui") ||
        !download_help.standard_output.contains("--theme")) {
        return fail("download help did not document TUI options");
    }
    const auto inspect_help =
        run_application(runtime_factory, {"onedrive-cpp", "inspect", "--help"});
    if (inspect_help.exit_code != 0 ||
        !inspect_help.standard_output.contains("health") ||
        !inspect_help.standard_output.contains("status") ||
        !inspect_help.standard_output.contains("drives") ||
        !inspect_help.standard_output.contains("shared") ||
        !inspect_help.standard_output.contains("sites") ||
        !inspect_help.standard_output.contains("quota") ||
        !inspect_help.standard_output.contains("storage") ||
        !inspect_help.standard_output.contains("partials") ||
        !inspect_help.standard_output.contains("files")) {
        return fail("inspect help did not list its read-only actions");
    }
    const auto health_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "health", "--help"}
    );
    if (health_help.exit_code != 0 ||
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
    if (status_help.exit_code != 0 ||
        !status_help.standard_output.contains(
            "Show read-only synchronization status"
        ) ||
        !status_help.standard_output.contains("--ui") ||
        !status_help.standard_output.contains("--theme")) {
        return fail("status help was not available");
    }
    const auto files_help = run_application(
        runtime_factory, {"onedrive-cpp", "inspect", "files", "--help"}
    );
    if (files_help.exit_code != 0 ||
        !files_help.standard_output.contains("[PATH]") ||
        !files_help.standard_output.contains("--status") ||
        !files_help.standard_output.contains("--ui")) {
        return fail("inspect files help was not available");
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
            .exit_code != 2) {
        return fail("inspect files accepted an invalid status");
    }
    const auto state_help =
        run_application(runtime_factory, {"onedrive-cpp", "state", "--help"});
    if (state_help.exit_code != 0 ||
        !state_help.standard_output.contains("reset-cursor") ||
        !state_help.standard_output.contains("clear")) {
        return fail("state help did not list its maintenance actions");
    }
    const auto reset_cursor_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "reset-cursor", "--help"}
    );
    if (reset_cursor_help.exit_code != 0 ||
        reset_cursor_help.standard_output.contains("--yes")) {
        return fail("state reset-cursor help exposed clear-only options");
    }
    const auto clear_state_help = run_application(
        runtime_factory, {"onedrive-cpp", "state", "clear", "--help"}
    );
    if (clear_state_help.exit_code != 0 ||
        !clear_state_help.standard_output.contains("--yes") ||
        clear_state_help.standard_output.contains("--clear-all")) {
        return fail("state clear help did not document its confirmation");
    }

    if (run_application(runtime_factory, {"onedrive-cpp", "unknown"})
            .exit_code != 2) {
        return fail("unknown command did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "login", "--config"}
        )
            .exit_code != 2) {
        return fail("missing option value did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "login", "--dry-run"}
        )
            .exit_code != 2) {
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
            .exit_code != 2) {
        return fail(
            "transfer watch accepted the one-shot large-delete override"
        );
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--log-level", "verbose"}
        )
            .exit_code != 2) {
        return fail("invalid log level did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--color", "sometimes"}
        )
            .exit_code != 2) {
        return fail("invalid color mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "account", "logout", "--output", "yaml"}
        )
            .exit_code != 2) {
        return fail("invalid output mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "transfer", "sync", "--ui", "graphical"}
        )
            .exit_code != 2) {
        return fail("invalid UI mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "transfer", "sync", "--theme", "rainbow"}
        )
            .exit_code != 2) {
        return fail("invalid TUI theme did not return usage exit code 2");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "state"}).exit_code !=
        2) {
        return fail("state command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "account"})
            .exit_code != 2) {
        return fail("account command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "inspect"})
            .exit_code != 2) {
        return fail("inspect command accepted a missing action");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "auth"}).exit_code !=
            2 ||
        run_application(runtime_factory, {"onedrive-cpp", "logout"})
                .exit_code != 2) {
        return fail("removed authentication commands were still accepted");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "state", "reset-cursor", "--yes"}
        )
            .exit_code != 2) {
        return fail("state reset-cursor accepted the clear confirmation flag");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "reset-state"})
            .exit_code != 2) {
        return fail("removed reset-state command was still accepted");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "transfer"})
            .exit_code != 2) {
        return fail("transfer command accepted a missing action");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "transfer", "download"}
        )
            .exit_code != 2) {
        return fail("transfer download accepted a missing remote path");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "inspect", "sites"}
        )
            .exit_code != 2) {
        return fail("inspect sites accepted a missing search query");
    }
    for (const std::string_view command :
         {"doctor", "status", "drives", "shared", "sites", "quota"}) {
        if (run_application(
                runtime_factory, {"onedrive-cpp", std::string{command}}
            )
                .exit_code != 2) {
            return fail("removed inspection command was still accepted");
        }
    }
    for (const std::string_view command : {"sync", "download", "monitor"}) {
        if (run_application(
                runtime_factory, {"onedrive-cpp", std::string{command}}
            )
                .exit_code != 2) {
            return fail("removed transfer command was still accepted");
        }
    }

    CliFixture ui_fixture;
    if (ui_fixture.authenticate().exit_code != 0) {
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
    if (configured_tui.exit_code != 1 || overridden_tui.exit_code != 0 ||
        download_tui.exit_code != 1) {
        return fail("CLI UI mode did not override configured TUI mode");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_args();
}
