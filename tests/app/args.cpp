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
        !sync_help.standard_output.contains("--force-large-delete")) {
        return fail("sync help did not document the large-delete override");
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
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_args();
}
