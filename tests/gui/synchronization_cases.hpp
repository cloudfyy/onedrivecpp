#pragma once

namespace onedrive::test::gui_sync {

void test_persistent_blocked_viewer();
void test_blocked_policy_descriptions();
void test_blocked_query_discards_stale_results();
void test_controller();
void test_default_controller_without_usable_credentials();
void test_controller_file_history();
void test_window_file_history();
void test_window_all_file_actions();
void test_controller_logging();
void test_controller_summaries_survive_log_overflow();
void test_controller_logging_after_failure();
void test_controller_lifecycle();
void test_history_bound();
void test_unsupported_format_reporting();
void test_session_logs_and_tabs();
void test_live_log_scroll();
void test_live_log_threshold();
void test_controller_preview_cache();
void test_preview_and_apply();
void test_progress_color();
void test_upload_progress_phases();
void test_progress_phases();
void test_controller_pending_delta_restart();
void test_readable_transfer_numbers();
void test_progress_and_stop();
void test_results_and_preflight();
void test_close_and_availability();
void test_shared_root_busy();

} // namespace onedrive::test::gui_sync
