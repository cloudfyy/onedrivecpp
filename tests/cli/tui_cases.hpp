#pragma once

#include <cstddef>

namespace onedrive::test::tui_cases {

int test_adaptive_sync_layout();
int test_file_history();
int test_inspection_completion();
int test_drive_details(std::size_t rows, int pages);
int test_drive_pagination();
int test_partial_list();
int test_drive_field_helpers();
int test_tui_dashboard();
int test_message_levels();

} // namespace onedrive::test::tui_cases
