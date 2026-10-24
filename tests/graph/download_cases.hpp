#pragma once

namespace onedrive::test::graph::download_cases {

int test_invalid_download_transport_options();
int test_download_account_authentication_failure();
int test_file_download_redirect();
int test_changed_file_download_is_not_retried();
int test_relaxed_file_download_ignores_remote_size();
int test_whole_file_download_rejects_error_body_before_retry();
int test_expired_download_redirect_is_refreshed();
int test_resumed_file_download();
int test_cancelled_download_is_not_retried_or_checkpointed();
int test_large_file_chunked_download();
int test_chunk_retry_resumes_from_durable_checkpoint();
int test_invalid_chunk_responses_are_rejected();

} // namespace onedrive::test::graph::download_cases
