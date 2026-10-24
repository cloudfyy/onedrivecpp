#include "support/progress.hpp"
#include "settings_cases.hpp"
#include "localization.hpp"
#include "support/common.hpp"
#include <QApplication>

using namespace onedrive;
using namespace onedrive::test::gui_settings;

int main(int argc, char** argv) {
    onedrive::test::ScenarioProgress progress{21};
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    try {
        progress.run(
            "tooltips_and_language_switching",
            test_tooltips_and_language_switching
        );
        progress.run(
            "tooltips_initial_locale_and_reload",
            test_tooltips_initial_locale_and_reload
        );
        progress.run(
            "tooltips_expanded_groups_and_disabled_drafts",
            test_tooltips_expanded_groups_and_disabled_drafts
        );
        progress.run("source_editing", test_source_editing);
        progress.run("round_trip", test_round_trip);
        progress.run("first_setup", test_first_setup);
        progress.run("inline_removal_and_utf8", test_inline_removal_and_utf8);
        progress.run(
            "editor_enum_reload_and_numeric_syntax",
            test_editor_enum_reload_and_numeric_syntax
        );
        progress.run(
            "numeric_limits_round_trip", test_numeric_limits_round_trip
        );
        progress.run(
            "editor_browse_and_advanced", test_editor_browse_and_advanced
        );
        progress.run(
            "configuration_fingerprint", test_configuration_fingerprint
        );
        progress.run(
            "persistence_rejects_without_writes",
            test_persistence_rejects_without_writes
        );
        progress.run(
            "cancel_save_and_import_errors", test_cancel_save_and_import_errors
        );
        progress.run(
            "import_creates_missing_configuration",
            test_import_creates_missing_configuration
        );
        progress.run("phase_two_round_trip", test_phase_two_round_trip);
        progress.run(
            "proxy_override_and_nested_shapes",
            test_proxy_override_and_nested_shapes
        );
        progress.run(
            "phase_two_enums_and_lower_limits",
            test_phase_two_enums_and_lower_limits
        );
        progress.run(
            "array_comment_preservation", test_array_comment_preservation
        );
        progress.run(
            "endpoint_import_rejection", test_endpoint_import_rejection
        );
        progress.run(
            "phase_two_proxy_drafts_and_empty_save",
            test_phase_two_proxy_drafts_and_empty_save
        );
        progress.run(
            "phase_two_patch_boundaries", test_phase_two_patch_boundaries
        );
        return progress.finish();
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
