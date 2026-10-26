#pragma once

namespace onedrive::test::gui_settings {

void test_tooltips_and_language_switching();
void test_tooltips_initial_locale_and_reload();
void test_tooltips_expanded_groups_and_disabled_drafts();
void test_round_trip();
void test_source_editing();
void test_first_setup();
void test_inline_removal_and_utf8();
void test_editor_enum_reload_and_numeric_syntax();
void test_numeric_limits_round_trip();
void test_editor_browse_and_advanced();
void test_sharepoint_library_browser();
void test_configuration_fingerprint();
void test_persistence_rejects_without_writes();
void test_cancel_save_and_import_errors();
void test_import_creates_missing_configuration();
void test_phase_two_round_trip();
void test_proxy_override_and_nested_shapes();
void test_phase_two_enums_and_lower_limits();
void test_array_comment_preservation();
void test_endpoint_import_rejection();
void test_phase_two_proxy_drafts_and_empty_save();
void test_phase_two_patch_boundaries();
void test_unsaved_settings_close_prompt();

} // namespace onedrive::test::gui_settings
