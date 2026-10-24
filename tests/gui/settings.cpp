#include "settings_cases.hpp"
#include "localization.hpp"
#include "support/common.hpp"
#include <QApplication>

using namespace onedrive;
using namespace onedrive::test::gui_settings;

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    try {
        test_tooltips_and_language_switching();
        test_tooltips_initial_locale_and_reload();
        test_tooltips_expanded_groups_and_disabled_drafts();
        test_source_editing();
        test_round_trip();
        test_first_setup();
        test_inline_removal_and_utf8();
        test_editor_enum_reload_and_numeric_syntax();
        test_numeric_limits_round_trip();
        test_editor_browse_and_advanced();
        test_configuration_fingerprint();
        test_persistence_rejects_without_writes();
        test_cancel_save_and_import_errors();
        test_import_creates_missing_configuration();
        test_phase_two_round_trip();
        test_proxy_override_and_nested_shapes();
        test_phase_two_enums_and_lower_limits();
        test_array_comment_preservation();
        test_endpoint_import_rejection();
        test_phase_two_proxy_drafts_and_empty_save();
        test_phase_two_patch_boundaries();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
