#include "support.hpp"

namespace {

using namespace onedrive::test::app;

int test_args() {
    FakeRuntimeFactory runtime_factory;

    const auto help =
        run_application(runtime_factory, {"build/release/onedrive-cpp"});
    if (help.exit_code != 0 || !help.standard_output.contains("auth") ||
        !help.standard_output.contains("drives") ||
        !help.standard_output.contains("shared") ||
        !help.standard_output.contains("sites") ||
        !help.standard_output.contains("quota") ||
        !help.standard_output.contains("status") ||
        !help.standard_output.contains("reset-state") ||
        !help.standard_output.contains("download") ||
        !help.standard_output.contains("sync") ||
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
    const auto sync_help =
        run_application(runtime_factory, {"onedrive-cpp", "sync", "--help"});
    if (sync_help.exit_code != 0 ||
        !sync_help.standard_output.contains("--force-large-delete") ||
        !sync_help.standard_output.contains("--ui") ||
        !sync_help.standard_output.contains("--theme")) {
        return fail("sync help did not document sync-specific options");
    }
    const auto monitor_help = run_application(
        runtime_factory, {"onedrive-cpp", "monitor", "--help"}
    );
    if (monitor_help.exit_code != 0 ||
        !monitor_help.standard_output.contains("--ui") ||
        !monitor_help.standard_output.contains("--theme")) {
        return fail("monitor help did not document TUI options");
    }
    const auto download_help = run_application(
        runtime_factory, {"onedrive-cpp", "download", "--help"}
    );
    if (download_help.exit_code != 0 ||
        !download_help.standard_output.contains("--ui") ||
        !download_help.standard_output.contains("--theme")) {
        return fail("download help did not document TUI options");
    }
    const auto doctor_help =
        run_application(runtime_factory, {"onedrive-cpp", "doctor", "--help"});
    if (doctor_help.exit_code != 0 ||
        !doctor_help.standard_output.contains(
            "Run local synchronization-state diagnostics"
        )) {
        return fail("doctor help was not available");
    }
    const auto status_help =
        run_application(runtime_factory, {"onedrive-cpp", "status", "--help"});
    if (status_help.exit_code != 0 ||
        !status_help.standard_output.contains(
            "Show read-only synchronization status"
        )) {
        return fail("status help was not available");
    }

    if (run_application(runtime_factory, {"onedrive-cpp", "unknown"})
            .exit_code != 2) {
        return fail("unknown command did not return usage exit code 2");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "auth", "--config"})
            .exit_code != 2) {
        return fail("missing option value did not return usage exit code 2");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "auth", "--dry-run"})
            .exit_code != 2) {
        return fail(
            "command-specific option was accepted by the wrong command"
        );
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "monitor", "--force-large-delete"}
        )
            .exit_code != 2) {
        return fail("monitor accepted the one-shot large-delete override");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "logout", "--log-level", "verbose"}
        )
            .exit_code != 2) {
        return fail("invalid log level did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "logout", "--color", "sometimes"}
        )
            .exit_code != 2) {
        return fail("invalid color mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "logout", "--output", "yaml"}
        )
            .exit_code != 2) {
        return fail("invalid output mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "sync", "--ui", "graphical"}
        )
            .exit_code != 2) {
        return fail("invalid UI mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "sync", "--theme", "rainbow"}
        )
            .exit_code != 2) {
        return fail("invalid TUI theme did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory, {"onedrive-cpp", "reset-state", "--yes"}
        )
            .exit_code != 2) {
        return fail("--yes was accepted without --clear-all");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "download"})
            .exit_code != 2) {
        return fail("download command accepted a missing remote path");
    }
    if (run_application(runtime_factory, {"onedrive-cpp", "sites"}).exit_code !=
        2) {
        return fail("sites command accepted a missing search query");
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
        color + std::string{"color = \"always\"\n"}.size(),
        "ui = \"tui\"\n"
    );
    onedrive::test::write_file(ui_fixture.config_path, configured);
    const auto configured_tui = run_application(
        ui_fixture.runtime_factory,
        {
            "onedrive-cpp",
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
            "download",
            "Documents/file.txt",
            "--config",
            ui_fixture.config_path.string(),
            "--dry-run",
        }
    );
    if (configured_tui.exit_code != 1 ||
        overridden_tui.exit_code != 0 ||
        download_tui.exit_code != 1) {
        return fail("CLI UI mode did not override configured TUI mode");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_args();
}
