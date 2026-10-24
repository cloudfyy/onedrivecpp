#include "support/progress.hpp"
#include "monitoring_support.hpp"
#include "monitoring_cases.hpp"

using namespace onedrive;
using namespace onedrive::test::gui_monitor;

int main(int argc, char* argv[]) {
    onedrive::test::ScenarioProgress progress{12};
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--monitor-child") {
            return child(argv[2], argv[3]);
        }
        QApplication application{argc, argv};
        application.setQuitOnLastWindowClosed(false);
        progress.run(
            "worker_exclusion_and_application_service",
            test_worker_exclusion_and_application_service
        );
        progress.run("owned_window", test_owned_window);
        progress.run("external_window", test_external_window);
        progress.run(
            "overlapping_root_is_not_attached",
            test_overlapping_root_is_not_attached
        );
        progress.run(
            "readonly_discovery_and_unsafe_endpoint",
            test_readonly_discovery_and_unsafe_endpoint
        );
        progress.run(
            "controller_configuration_generations",
            test_controller_configuration_generations
        );
        progress.run(
            "controller_missing_target_and_stale_endpoint",
            test_controller_missing_target_and_stale_endpoint
        );
        progress.run("readonly_owner_errors", test_readonly_owner_errors);
        progress.run(
            "external_replacement_during_confirmation",
            test_external_replacement_during_confirmation
        );
        progress.run("readable_monitor_results", test_readable_monitor_results);
        progress.run("monitor_session_totals", test_monitor_session_totals);
        progress.run(
            "polling_reuses_selected_directory_without_config_logs",
            test_polling_reuses_selected_directory_without_config_logs
        );
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return progress.finish();
}
