#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

using namespace onedrive;
using namespace onedrive::test::gui_monitor;

int main(int argc, char* argv[]) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--monitor-child") {
            return child(argv[2], argv[3]);
        }
        QApplication application{argc, argv};
        application.setQuitOnLastWindowClosed(false);
        test_worker_exclusion_and_application_service();
        test_owned_window();
        test_external_window();
        test_overlapping_root_is_not_attached();
        test_readonly_discovery_and_unsafe_endpoint();
        test_controller_configuration_generations();
        test_controller_missing_target_and_stale_endpoint();
        test_readonly_owner_errors();
        test_external_replacement_during_confirmation();
        test_readable_monitor_results();
        test_monitor_session_totals();
        test_polling_reuses_selected_directory_without_config_logs();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
