#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "app/preflight.hpp"
#include "onedrive/logging/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::app;

int test_multiple_drive_preflight() {
    using Operation = onedrive::app::detail::Operation;
    CliFixture fixture;
    auto config = onedrive::config::Config::load(fixture.config_path);
    config.drive_ids = {"first", "second"};
    for (const auto operation :
         {Operation::download,
          Operation::reset_cursor,
          Operation::clear_state,
          Operation::cleanup_state,
          Operation::verify,
          Operation::shared}) {
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    const onedrive::app::detail::RuntimePreflight preflight{
                        config, operation
                    };
                },
                "this operation requires one Drive"
            )) {
            return fail(
                "an ambiguous operation accepted multiple Drive targets"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_logout_without_credentials() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const auto logout = [&] {
        return run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "logout",
             "--config",
             fixture.config_path.string(),
             "--color",
             "never"}
        );
    };
    auto result = logout();
    if (result.exit_code != exit_code::success ||
        !result.standard_output.contains(
            "No saved authentication was present."
        ) ||
        fixture.runtime_factory.token_store_count != 0 ||
        std::filesystem::exists(fixture.state_path / "active_account")) {
        return fail("logout without an active account accessed credentials");
    }
    const auto paths = [&] {
        const onedrive::logging::Session logging{{}};
        return onedrive::account::AccountState::activate(
            fixture.state_path,
            onedrive::test::test_drive_identity("me"),
            "saved-token"
        );
    }();
    const auto token = paths.token_directory / "refresh_token";
    const auto account_metadata =
        onedrive::test::read_file(paths.token_directory / "account.json");
    {
        const onedrive::logging::Session logging{{}};
        const onedrive::app::detail::RuntimePreflight busy{
            config, onedrive::app::detail::Operation::logout
        };
        result = logout();
        if (result.exit_code != exit_code::failure ||
            !result.standard_error.contains("another onedrive-cpp process") ||
            result.standard_output.contains("Saved authentication removed") ||
            onedrive::test::read_file(token) != "saved-token" ||
            fixture.runtime_factory.token_store_count != 0) {
            return fail(
                "CLI logout did not preserve credentials under a runtime lock"
            );
        }
    }
    std::filesystem::remove(token);
    result = logout();
    if (result.exit_code != exit_code::success ||
        !result.standard_output.contains(
            "No saved authentication was present."
        ) ||
        fixture.runtime_factory.token_store_count != 1 ||
        onedrive::test::read_file(paths.token_directory / "account.json") !=
            account_metadata) {
        return fail("logout without a refresh token changed account metadata");
    }
    return EXIT_SUCCESS;
}

int test_log_level_thresholds() {
    const std::vector markers{
        "cli threshold trace", "cli threshold debug", "cli threshold info",
        "cli threshold warning", "cli threshold error", "cli threshold critical"
    };
    for (const auto& [threshold, first] :
         {std::pair{"trace", 0U}, std::pair{"debug", 1U},
          std::pair{"info", 2U}, std::pair{"warn", 3U},
          std::pair{"error", 4U}, std::pair{"critical", 5U},
          std::pair{"off", 6U}}) {
        CliFixture fixture;
        bool emitted = false;
        fixture.runtime_factory.authentication_response_override =
            [&](const onedrive::http::HttpRequest&)
            -> std::optional<onedrive::http::HttpResult> {
                if (!std::exchange(emitted, true)) {
                    spdlog::trace("{}", markers[0]);
                    spdlog::debug("{}", markers[1]);
                    spdlog::info("{}", markers[2]);
                    spdlog::warn("{}", markers[3]);
                    spdlog::error("{}", markers[4]);
                    spdlog::critical("{}", markers[5]);
                }
                return std::nullopt;
            };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp", "account", "login", "--config",
             fixture.config_path.string(), "--ui", "console",
             "--output", "json", "--log-level", threshold}
        );
        if (result.exit_code != exit_code::success || !emitted) {
            return fail("CLI logging threshold changed the command outcome");
        }
        const auto logged = onedrive::test::read_file(fixture.log_path);
        for (std::size_t index = 0; index < markers.size(); ++index) {
            if (logged.contains(markers[index]) != (index >= first)) {
                return fail(
                    "CLI --log-level did not retain its severity and all higher levels"
                );
            }
        }
        std::istringstream output{result.standard_output};
        std::string line;
        bool completed = false;
        while (std::getline(output, line)) {
            const auto event = nlohmann::json::parse(line);
            if (event.at("event") == "authentication_succeeded") {
                completed = event.at("level") == "info";
            }
        }
        if (!completed || (first == markers.size() && !logged.empty())) {
            return fail(
                "CLI log filtering suppressed command results or ignored logging off"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_flow() {
    CliFixture fixture;
    auto& temporary_directory = fixture.temporary_directory;
    const auto& config_path = fixture.config_path;
    const auto& state_path = fixture.state_path;
    const auto& sync_path = fixture.sync_path;
    const auto& log_path = fixture.log_path;
    auto& runtime_factory = fixture.runtime_factory;
    const auto authentication = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "account",
            "login",
            "--config",
            config_path.string(),
        }
    );
    const auto account_path =
        onedrive::account::AccountState::active_token_directory(state_path);
    if (authentication.exit_code != exit_code::success ||
        !authentication.standard_output.contains("\x1b[") ||
        !authentication.standard_output.contains("Authentication succeeded") ||
        !authentication.standard_output.contains("business") ||
        !authentication.standard_output.contains(
            "WARNING: Authentication requests broad organizational"
        ) ||
        !account_path.filename().string().starts_with("Test-User--") ||
        !std::filesystem::exists(account_path / "avatar.jpg") ||
        !std::filesystem::exists(account_path / "account.json") ||
        !std::filesystem::exists(account_path / "refresh_token") ||
        std::filesystem::directory_iterator{account_path / "drives"} ==
            std::filesystem::directory_iterator{}) {
        return fail("authentication did not initialize account state");
    }

    const auto logout = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "account",
            "logout",
            "--config",
            config_path.string(),
            "--color",
            "never",
        }
    );
    if (logout.exit_code != exit_code::success ||
        logout.standard_output.contains("\x1b[") ||
        !logout.standard_output.contains("Saved authentication removed") ||
        runtime_factory.token_store_count != 1) {
        return fail("logout did not accept a subcommand configuration path");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains("Removing locally saved authentication")) {
            return fail("configured log file did not receive application logs");
        }
    }
    if (run_application(
            runtime_factory,
            {
                "onedrive-cpp",
                "account",
                "login",
                "--config",
                config_path.string(),
            }
        )
            .exit_code != exit_code::success) {
        return fail("reauthentication did not restore account state");
    }

    const auto drives = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "drives",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (drives.exit_code != exit_code::success ||
        !drives.standard_output.contains(R"("event":"drive")") ||
        !drives.standard_output.contains(R"("id":"drive-id")") ||
        !drives.standard_output.contains(R"("configured":"true")") ||
        !drives.standard_output.contains(R"("id":"shared-drive-id")") ||
        drives.standard_output.find(
            R"("id":"drive-id")",
            drives.standard_output.find(R"("id":"drive-id")") + 1
        ) != std::string::npos) {
        return fail("drives command did not report available drives");
    }

    const auto shared = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "shared",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (shared.exit_code != exit_code::success ||
        !shared.standard_output.contains(R"("event":"shared_resource")") ||
        !shared.standard_output.contains(R"("source":"shared_with_me")") ||
        !shared.standard_output.contains(R"("source":"shortcut")") ||
        !shared.standard_output.contains(R"("drive_id":"team-drive-id")") ||
        !shared.standard_output.contains(R"("local_path":"Team Shortcut")")) {
        return fail("shared command did not report shared resources");
    }

    const auto sites = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "sites",
            "Engineering",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (sites.exit_code != exit_code::success ||
        !sites.standard_output.contains(R"("event":"site")") ||
        !sites.standard_output.contains(R"("name":"Engineering")") ||
        !sites.standard_output.contains(R"("event":"site_drive")") ||
        !sites.standard_output.contains(R"("id":"library-drive-id")")) {
        return fail("sites command did not report document libraries");
    }
    runtime_factory.empty_shared = true;
    const auto no_shared = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "shared",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    runtime_factory.empty_shared = false;
    if (no_shared.exit_code != exit_code::success ||
        !no_shared.standard_output.contains(
                                        R"("event":"no_shared_resources")"
                                    )) {
        return fail("shared command did not report an empty result");
    }
    runtime_factory.empty_sites = true;
    const auto no_sites = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "sites",
            "Engineering",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    runtime_factory.empty_sites = false;
    if (no_sites.exit_code != exit_code::success ||
        !no_sites.standard_output.contains(
                                       R"("event":"no_sharepoint_sites")"
                                   )) {
        return fail("sites command did not report an empty result");
    }

    const auto quota = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "quota",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (quota.exit_code != exit_code::success ||
        !quota.standard_output.contains(R"("event":"quota")") ||
        !quota.standard_output.contains(R"("total":"2.00 KiB")") ||
        !quota.standard_output.contains(R"("remaining":"600 B")") ||
        !quota.standard_output.contains(R"("state":"normal")")) {
        return fail("quota command did not report configured drive quota");
    }

    const auto status = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "status",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (status.exit_code != exit_code::success ||
        !status.standard_output.contains(R"("event":"status")") ||
        !status.standard_output.contains(R"("account":"Test User")") ||
        !status.standard_output.contains(R"("drive_type":"business")") ||
        !status.standard_output.contains(R"("state_database":"absent")") ||
        !status.standard_output.contains(R"("last_sync":"never")") ||
        runtime_factory.graph_info_client_count != 7) {
        return fail("status command did not report read-only local state");
    }
    const auto identity = onedrive::test::test_drive_identity("drive-id");
    const auto paths =
        onedrive::account::AccountState::locate(state_path, identity);
    onedrive::metrics::FileMetrics{paths.drive_directory}.record_sync_run(
        onedrive::metrics::SyncRunOutcome::failed,
        std::chrono::milliseconds{250}
    );
    const auto failed_status = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "status",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (failed_status.exit_code != exit_code::success ||
        !failed_status.standard_output.contains(R"("last_result":"failed")") ||
        failed_status.standard_output.contains(R"("last_sync":"never")") ||
        runtime_factory.graph_info_client_count != 8) {
        return fail("status command did not report the last synchronization");
    }

    const auto reset_state = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "reset-cursor",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        }
    );
    if (reset_state.exit_code != exit_code::success ||
        !reset_state.standard_output.contains(
            "Reset synchronization cursor for drive 'me' (drive-id): saved "
            "cursor removed"
        ) ||
        !reset_state.standard_output.contains(
            "Item snapshots, pending downloads, partial downloads, pending "
            "uploads, pending moves, upload suppressions, and blocked items "
            "were preserved"
        ) ||
        !reset_state.standard_output.contains(
            "next sync will perform a full Microsoft Graph delta query"
        ) ||
        runtime_factory.item_store_count != 1 ||
        runtime_factory.item_store_open_count != 1 ||
        runtime_factory.item_store_reset_count != 1 ||
        runtime_factory.reset_drive_id != "drive-id" ||
        runtime_factory.graph_client_count != 1 ||
        runtime_factory.metrics_count != 0) {
        return fail("state reset-cursor was not dispatched to the item store");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Synchronization cursor reset completed for drive 'drive-id': "
                "saved "
                "cursor removed; item snapshots, pending downloads, partial "
                "downloads, pending uploads, pending moves, upload "
                "suppressions, and blocked items preserved; next sync will "
                "use an initial delta query"
            )) {
            return fail(
                "state reset-cursor completion was not written to the log"
            );
        }
    }

    const auto cancelled_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "clear",
            "--config",
            config_path.string(),
        },
        "wrong-drive\n"
    );
    if (cancelled_clear.exit_code != exit_code::failure ||
        !cancelled_clear.standard_output.contains("WARNING:") ||
        !cancelled_clear.standard_output.contains(
            "Full state clear cancelled"
        ) ||
        runtime_factory.clear_count_ != 0 ||
        runtime_factory.item_store_count != 1) {
        return fail("full state clear accepted an invalid confirmation");
    }

    const auto confirmed_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "clear",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        },
        "me\n"
    );
    if (confirmed_clear.exit_code != exit_code::success ||
        !confirmed_clear.standard_output.contains("drive 'me' (drive-id)") ||
        !confirmed_clear.standard_output.contains(
            "Type the configured drive reference 'me'"
        ) ||
        !confirmed_clear.standard_output.contains(
            "7 item snapshots, 2 pending downloads, 4 partial downloads, 1 "
            "pending uploads, 5 pending moves, 6 upload suppressions, and 3 "
            "blocked items removed"
        ) ||
        !confirmed_clear.standard_output.contains(
            "Cleared all synchronization state for drive 'me' (drive-id)"
        ) ||
        !confirmed_clear.standard_output.contains(
            "Local files were not deleted"
        ) ||
        runtime_factory.clear_count_ != 1 ||
        runtime_factory.clear_drive_id_ != "drive-id" ||
        runtime_factory.item_store_count != 2 ||
        runtime_factory.item_store_open_count != 2) {
        return fail("confirmed full state clear was not executed");
    }

    const auto automated_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "clear",
            "--yes",
            "--config",
            config_path.string(),
        }
    );
    if (automated_clear.exit_code != exit_code::success ||
        automated_clear.standard_output.contains(
            "Type the configured drive reference"
        ) ||
        runtime_factory.clear_count_ != 2 ||
        runtime_factory.item_store_count != 3 ||
        runtime_factory.item_store_open_count != 3) {
        return fail("--yes did not explicitly confirm full state clear");
    }

    const auto dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--log-level",
            "debug",
            "--log-file",
            log_path.string(),
        }
    );
    const auto expected_sync_directory =
        onedrive::account::AccountState::drive_data_directory(
            sync_path, onedrive::test::test_drive_identity()
        );
    if (dry_run.exit_code != exit_code::success ||
        !dry_run.standard_output.contains("Dry run configuration") ||
        !dry_run.standard_output.contains(
            "Remote delta contains 1 changes (1 upserts, 0 removals, 0 moves, "
            "0 blocked)"
        ) ||
        !dry_run.standard_output.contains("throttle retries:     6") ||
        !dry_run.standard_output.contains(
            "throttle delay:       3-120 seconds"
        ) ||
        !dry_run.standard_output.contains("download concurrency: 4") ||
        !dry_run.standard_output.contains("per-download rate:    0") ||
        !dry_run.standard_output.contains("total download rate:  0") ||
        runtime_factory.item_store_count != 4 ||
        runtime_factory.item_store_open_count != 4 ||
        runtime_factory.item_store_apply_delta_count != 0 ||
        runtime_factory.graph_client_count != 5 ||
        runtime_factory.metrics_count != 1 ||
        runtime_factory.last_item_store_sync_directory !=
            expected_sync_directory ||
        !dry_run.standard_output.contains(expected_sync_directory.string())) {
        return fail("sync dry-run command was not parsed or executed");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Preparing Microsoft Graph delta query for drive 'drive-id': 0 "
                "tracked "
                "items, saved cursor absent"
            ) ||
            !contents.contains(
                "Remote delta prepared for drive 'drive-id': 1 upserts, 0 "
                "removals"
            ) ||
            !contents.contains(
                "Synchronization plan for drive 'drive-id': 0 directories, 1 "
                "downloads, 42 bytes, 0 deferred local removals"
            ) ||
            !contents.contains(
                "Dry run preserved files, snapshots, and cursor; saved blocked "
                "diagnostics for drive 'drive-id'"
            ) ||
            !contents.contains("Synchronization dry run completed")) {
            return fail("sync dry-run diagnostics were not written to the log");
        }
    }

    const std::string single_remote_path{"Documents/single file.txt"};
    const auto single_destination =
        expected_sync_directory / single_remote_path;
    const int stores_before_single_download = runtime_factory.item_store_count;
    const auto single_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
            "--dry-run",
        }
    );
    if (single_dry_run.exit_code != exit_code::success ||
        !single_dry_run.standard_output.contains("Single-file download plan") ||
        std::filesystem::exists(single_destination) ||
        runtime_factory.item_store_count != stores_before_single_download + 1 ||
        runtime_factory.item_store_apply_delta_count != 0) {
        return fail("single-file download dry run changed local state");
    }

    const auto single_download = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
        }
    );
    if (single_download.exit_code != exit_code::success ||
        !single_download.standard_output.contains(
            "Downloaded 'Documents/single file.txt'"
        ) ||
        !std::filesystem::is_regular_file(single_destination) ||
        std::filesystem::file_size(single_destination) != 42 ||
        runtime_factory.item_store_count != stores_before_single_download + 2 ||
        runtime_factory.item_store_apply_delta_count != 0) {
        return fail("single-file download was not safely dispatched");
    }

    const auto legacy_file = sync_path / "legacy.txt";
    std::filesystem::create_directories(sync_path);
    {
        std::ofstream output{legacy_file};
        output << "legacy";
    }
    const auto trace_sync = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            config_path.string(),
            "--log-level",
            "trace",
            "--log-file",
            log_path.string(),
        }
    );
    if (trace_sync.exit_code != exit_code::success ||
        !trace_sync.standard_output.contains(
            "Remote delta contains 1 changes (1 upserts, 0 removals, 0 moves, "
            "0 blocked)"
        ) ||
        runtime_factory.item_store_apply_delta_count != 1) {
        return fail("sync trace command did not apply the remote delta");
    }
    if (!std::filesystem::is_regular_file(
            expected_sync_directory / "notes.txt"
        )) {
        return fail("synchronized file was not isolated by account and Drive");
    }
    if (std::filesystem::exists(sync_path / "notes.txt")) {
        return fail("synchronized file was written to the flat data root");
    }
    if (!std::filesystem::is_regular_file(legacy_file)) {
        return fail("legacy flat-layout file was modified by synchronization");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        const auto queued =
            contents.find("Queued 'notes.txt' for download (42 bytes)");
        const auto executing =
            contents.find("Executing 1 downloads with concurrency");
        const auto downloading =
            contents.find("Downloading 'notes.txt' (42 bytes)");
        if (!contents.contains(
                "Remote delta prepared for drive 'drive-id': 1 upserts, 0 "
                "removals"
            ) ||
            !contents.contains(
                "Persisting remote delta for drive 'drive-id'"
            ) ||
            queued == std::string::npos || executing == std::string::npos ||
            downloading == std::string::npos ||
            !(queued < executing && executing < downloading) ||
            !contents.contains("Atomically installed 'notes.txt' (42 bytes)") ||
            !contents.contains(
                "Download execution completed: 1 downloaded, 0 reused, 0 "
                "directories prepared"
            ) ||
            !contents.contains(
                "Remote item changed: path='notes.txt', id='file-id', "
                "eTag='file-etag', type=file"
            )) {
            return fail(
                "remote delta item metadata was not written at trace level"
            );
        }
    }

    const auto json_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--output",
            "json",
            "--color",
            "always",
        }
    );
    if (json_dry_run.exit_code != exit_code::success ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"dry_run_configuration\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"synchronization_plan\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"sync_completed\""
        ) ||
        !json_dry_run.standard_output.contains("\"event\":\"delta_summary\"") ||
        !json_dry_run.standard_output.contains(
            "\"message\":\"Fetching Microsoft Graph changes...\""
        ) ||
        !json_dry_run.standard_output.contains("Microsoft Graph") ||
        json_dry_run.standard_output.contains("\033[")) {
        return fail("JSON output was not emitted as unstyled JSON Lines");
    }

    const auto quiet_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--quiet",
        }
    );
    if (quiet_dry_run.exit_code != exit_code::success ||
        !quiet_dry_run.standard_output.empty()) {
        return fail("quiet output did not suppress sync information");
    }

    const auto monitor = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "transfer",
            "watch",
            "--config",
            config_path.string(),
        }
    );
    if (monitor.exit_code != exit_code::success ||
        runtime_factory.monitor_count != 1 ||
        runtime_factory.monitor_run_count != 1 ||
        runtime_factory.monitor_sync_count != 1 ||
        !monitor.standard_output.contains(
            "Monitoring local and Microsoft Graph changes"
        ) ||
        monitor.standard_output.contains("scaffold")) {
        return fail(
            "monitor command did not reuse synchronization through the "
            "runtime factory"
        );
    }

    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{4};
    if (const auto result = progress.run(
            "multiple_drive_preflight", test_multiple_drive_preflight
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "logout_without_credentials", test_logout_without_credentials
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("log_level_thresholds", test_log_level_thresholds);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run("flow", test_flow));
}
