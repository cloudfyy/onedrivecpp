#include "support.hpp"

namespace {

using namespace onedrive::test::app;

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
            "auth",
            "--config",
            config_path.string(),
        }
    );
    const auto account_path =
        onedrive::account::AccountState::active_token_directory(state_path);
    if (authentication.exit_code != 0 ||
        !authentication.standard_output.contains("\x1b[") ||
        !authentication.standard_output.contains("Authentication succeeded") ||
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
            "logout",
            "--config",
            config_path.string(),
            "--color",
            "never",
        }
    );
    if (logout.exit_code != 0 || logout.standard_output.contains("\x1b[") ||
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
                "auth",
                "--config",
                config_path.string(),
            }
        )
            .exit_code != 0) {
        return fail("reauthentication did not restore account state");
    }

    const auto drives = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "drives",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (drives.exit_code != 0 ||
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
            "shared",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (shared.exit_code != 0 ||
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
            "sites",
            "Engineering",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (sites.exit_code != 0 ||
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
            "shared",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    runtime_factory.empty_shared = false;
    if (no_shared.exit_code != 0 || !no_shared.standard_output.contains(
                                        R"("event":"no_shared_resources")"
                                    )) {
        return fail("shared command did not report an empty result");
    }
    runtime_factory.empty_sites = true;
    const auto no_sites = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sites",
            "Engineering",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    runtime_factory.empty_sites = false;
    if (no_sites.exit_code != 0 || !no_sites.standard_output.contains(
                                       R"("event":"no_sharepoint_sites")"
                                   )) {
        return fail("sites command did not report an empty result");
    }

    const auto quota = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "quota",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (quota.exit_code != 0 ||
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
            "status",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (status.exit_code != 0 ||
        !status.standard_output.contains(R"("event":"status")") ||
        !status.standard_output.contains(R"("account":"Test User")") ||
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
            "status",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (failed_status.exit_code != 0 ||
        !failed_status.standard_output.contains(R"("last_result":"failed")") ||
        failed_status.standard_output.contains(R"("last_sync":"never")") ||
        runtime_factory.graph_info_client_count != 8) {
        return fail("status command did not report the last synchronization");
    }

    const auto reset_state = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        }
    );
    if (reset_state.exit_code != 0 ||
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
            "next sync will perform a full cloud check"
        ) ||
        runtime_factory.item_store_count != 1 ||
        runtime_factory.item_store_open_count != 1 ||
        runtime_factory.item_store_reset_count != 1 ||
        runtime_factory.reset_drive_id != "drive-id" ||
        runtime_factory.graph_client_count != 1 ||
        runtime_factory.metrics_count != 0) {
        return fail("reset-state command was not dispatched to the item store");
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
            return fail("reset-state completion was not written to the log");
        }
    }

    const auto cancelled_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--clear-all",
            "--config",
            config_path.string(),
        },
        "wrong-drive\n"
    );
    if (cancelled_clear.exit_code != 1 ||
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
            "reset-state",
            "--clear-all",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        },
        "me\n"
    );
    if (confirmed_clear.exit_code != 0 ||
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
            "reset-state",
            "--clear-all",
            "--yes",
            "--config",
            config_path.string(),
        }
    );
    if (automated_clear.exit_code != 0 ||
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
    if (dry_run.exit_code != 0 ||
        !dry_run.standard_output.contains("Dry run configuration") ||
        !dry_run.standard_output.contains(
            "Cloud storage has 1 changes (1 new or updated, 0 removed, 0 "
            "moved, 0 need attention)"
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
                "Dry run left synchronization state unchanged for drive "
                "'drive-id'"
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
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
            "--dry-run",
        }
    );
    if (single_dry_run.exit_code != 0 ||
        !single_dry_run.standard_output.contains("Single-file download plan") ||
        std::filesystem::exists(single_destination) ||
        runtime_factory.item_store_count != stores_before_single_download ||
        runtime_factory.item_store_apply_delta_count != 0) {
        return fail("single-file download dry run changed local state");
    }

    const auto single_download = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
        }
    );
    if (single_download.exit_code != 0 ||
        !single_download.standard_output.contains(
            "Downloaded 'Documents/single file.txt'"
        ) ||
        !std::filesystem::is_regular_file(single_destination) ||
        std::filesystem::file_size(single_destination) != 42 ||
        runtime_factory.item_store_count != stores_before_single_download + 1 ||
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
            "sync",
            "--config",
            config_path.string(),
            "--log-level",
            "trace",
            "--log-file",
            log_path.string(),
        }
    );
    if (trace_sync.exit_code != 0 ||
        !trace_sync.standard_output.contains(
            "Cloud storage has 1 changes (1 new or updated, 0 removed, 0 "
            "moved, 0 need attention)"
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
        const auto queued = contents.find(
            "Queued 'notes.txt' for download (42 bytes)"
        );
        const auto executing = contents.find(
            "Executing 1 downloads with concurrency"
        );
        const auto downloading = contents.find(
            "Downloading 'notes.txt' (42 bytes)"
        );
        if (!contents.contains(
                "Remote delta prepared for drive 'drive-id': 1 upserts, 0 "
                "removals"
            ) ||
            !contents.contains(
                "Persisting remote delta for drive 'drive-id'"
            ) ||
            queued == std::string::npos ||
            executing == std::string::npos ||
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
    if (json_dry_run.exit_code != 0 ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"dry_run_configuration\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"synchronization_plan\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"sync_completed\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"delta_summary\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"message\":\"Checking the cloud for changes...\""
        ) ||
        json_dry_run.standard_output.contains("Microsoft Graph") ||
        json_dry_run.standard_output.contains("\033[")) {
        return fail("JSON output was not emitted as unstyled JSON Lines");
    }

    const auto quiet_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--quiet",
        }
    );
    if (quiet_dry_run.exit_code != 0 ||
        !quiet_dry_run.standard_output.empty()) {
        return fail("quiet output did not suppress sync information");
    }

    const auto monitor = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "monitor",
            "--config",
            config_path.string(),
        }
    );
    if (monitor.exit_code != 0 || runtime_factory.monitor_count != 1 ||
        runtime_factory.monitor_run_count != 1 ||
        runtime_factory.monitor_sync_count != 1 ||
        !monitor.standard_output.contains(
            "Watching for local and cloud changes"
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
    return test_flow();
}
