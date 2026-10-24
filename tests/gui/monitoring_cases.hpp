#pragma once

#include <filesystem>

namespace onedrive::test::gui_monitor {

int child(
    const std::filesystem::path& file, const std::filesystem::path& gates
);

void test_worker_exclusion_and_application_service();
void test_owned_window();
void test_external_window();
void test_monitor_session_totals();
void test_overlapping_root_is_not_attached();
void test_readonly_discovery_and_unsafe_endpoint();
void test_controller_configuration_generations();
void test_controller_missing_target_and_stale_endpoint();
void test_readonly_owner_errors();
void test_external_replacement_during_confirmation();
void test_polling_reuses_selected_directory_without_config_logs();
void test_readable_monitor_results();

} // namespace onedrive::test::gui_monitor
