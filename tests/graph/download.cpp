#include "support.hpp"
#include "download_cases.hpp"

using namespace onedrive::test::graph;
using namespace onedrive::test::graph::download_cases;

int main() {
    if (const int result = test_download_account_authentication_failure();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_download_transport_options();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_file_download_redirect();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_changed_file_download_is_not_retried();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_relaxed_file_download_ignores_remote_size();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_whole_file_download_rejects_error_body_before_retry();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_expired_download_redirect_is_refreshed();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_resumed_file_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_cancelled_download_is_not_retried_or_checkpointed();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_large_file_chunked_download();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_chunk_retry_resumes_from_durable_checkpoint();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_chunk_responses_are_rejected();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
