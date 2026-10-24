#pragma once

namespace onedrive::test::auth::diagnostics_cases {

int test_enterprise_error_diagnostics();
int test_error_response_fallbacks();
int test_signed_diagnostic_code();
int test_malformed_error_diagnostics();
int test_diagnostic_codes_do_not_control_polling();

} // namespace onedrive::test::auth::diagnostics_cases
