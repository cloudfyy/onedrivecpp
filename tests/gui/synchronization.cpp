#include "synchronization_cases.hpp"
#include "localization.hpp"
#include "support/common.hpp"
#include <QApplication>

using namespace onedrive;
using namespace onedrive::test::gui_sync;

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    application.setQuitOnLastWindowClosed(false);
    gui::Localization localization{gui::Language::english};
    try {
        test_controller();
        test_controller_file_history();
        test_window_file_history();
        test_window_all_file_actions();
        test_persistent_blocked_viewer();
        test_blocked_policy_descriptions();
        test_blocked_query_discards_stale_results();
        test_controller_logging();
        test_controller_summaries_survive_log_overflow();
        test_controller_logging_after_failure();
        test_default_controller_without_usable_credentials();
        test_controller_lifecycle();
        test_history_bound();
        test_session_logs_and_tabs();
        test_unsupported_format_reporting();
        test_live_log_threshold();
        test_live_log_scroll();
        test_controller_preview_cache();
        test_controller_pending_delta_restart();
        test_preview_and_apply();
        test_progress_color();
        test_progress_phases();
        test_upload_progress_phases();
        test_readable_transfer_numbers();
        test_progress_and_stop();
        test_results_and_preflight();
        test_shared_root_busy();
        test_close_and_availability();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
