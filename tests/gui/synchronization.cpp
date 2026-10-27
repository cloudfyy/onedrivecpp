#include "support/progress.hpp"
#include "synchronization_cases.hpp"
#include "localization.hpp"
#include "support/common.hpp"
#include "autostart.hpp"
#include <QApplication>

using namespace onedrive;
using namespace onedrive::test::gui_sync;

int main(int argc, char* argv[]) {
    onedrive::test::ScenarioProgress progress{36};
    QApplication application{argc, argv};
    application.setQuitOnLastWindowClosed(false);
    gui::Localization localization{gui::Language::english};
    try {
        if (application.arguments() ==
            QStringList{
                application.arguments().front(), "--write-autostart-entry"
            }) {
            gui::set_autostart_enabled(true);
            return EXIT_SUCCESS;
        }
        progress.run("controller", test_controller);
        progress.run("controller_file_history", test_controller_file_history);
        progress.run("window_file_history", test_window_file_history);
        progress.run("window_all_file_actions", test_window_all_file_actions);
        progress.run(
            "persistent_blocked_viewer", test_persistent_blocked_viewer
        );
        progress.run(
            "blocked_policy_descriptions", test_blocked_policy_descriptions
        );
        progress.run(
            "blocked_query_discards_stale_results",
            test_blocked_query_discards_stale_results
        );
        progress.run("controller_logging", test_controller_logging);
        progress.run(
            "controller_summaries_survive_log_overflow",
            test_controller_summaries_survive_log_overflow
        );
        progress.run(
            "controller_logging_after_failure",
            test_controller_logging_after_failure
        );
        progress.run(
            "default_controller_without_usable_credentials",
            test_default_controller_without_usable_credentials
        );
        progress.run("controller_lifecycle", test_controller_lifecycle);
        progress.run("history_bound", test_history_bound);
        progress.run("session_logs_and_tabs", test_session_logs_and_tabs);
        progress.run(
            "unsupported_format_reporting", test_unsupported_format_reporting
        );
        progress.run("live_log_threshold", test_live_log_threshold);
        progress.run("live_log_scroll", test_live_log_scroll);
        progress.run("controller_preview_cache", test_controller_preview_cache);
        progress.run(
            "controller_pending_delta_restart",
            test_controller_pending_delta_restart
        );
        progress.run("preview_and_apply", test_preview_and_apply);
        progress.run("progress_color", test_progress_color);
        progress.run("progress_phases", test_progress_phases);
        progress.run("upload_progress_phases", test_upload_progress_phases);
        progress.run(
            "readable_transfer_numbers", test_readable_transfer_numbers
        );
        progress.run("progress_and_stop", test_progress_and_stop);
        progress.run("results_and_preflight", test_results_and_preflight);
        progress.run("shared_root_busy", test_shared_root_busy);
        progress.run("close_and_availability", test_close_and_availability);
        progress.run("tray_window_lifecycle", test_tray_window_lifecycle);
        progress.run("tray_unsaved_settings", test_tray_unsaved_settings);
        progress.run("tray_graceful_quit", test_tray_graceful_quit);
        progress.run("tray_startup_visibility", test_tray_startup_visibility);
        progress.run("autostart_settings", test_autostart_settings);
        progress.run("autostart_entry_failures", test_autostart_entry_failures);
        progress.run(
            "autostart_executable_quoting", test_autostart_executable_quoting
        );
        progress.run("tray_folder_failures", test_tray_folder_failures);
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return progress.finish();
}
