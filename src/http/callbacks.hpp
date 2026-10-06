#pragma once

#include "onedrive/http/http_client.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <exception>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::http::detail {

struct DownloadState {
    std::uint64_t durable_offset{};
    std::uint64_t file_offset{};
    bool response_accepted{true};
    bool response_validated{false};
};

enum class CallbackFailureKind {
    none,
    response_too_large,
    write,
    throttle,
    data_callback,
    allocation,
    internal,
};

enum class CallbackStorage {
    response_body,
    response_headers,
};

struct CallbackFailure {
    CallbackFailureKind kind{CallbackFailureKind::none};
    int system_error{};
    std::string detail;
};

enum class ResponseAcceptance {
    pending,
    accepted,
    rejected,
};

struct WriteContext {
    std::string body;
    std::size_t maximum_size;
    int descriptor{-1};
    CallbackFailure failure;
    std::size_t received_size{};
    std::uint64_t file_offset{};
    const DownloadData* download_data{};
    const DownloadCheckpoint* checkpoint{};
    const DownloadResponseGate* response_gate{};
    const DownloadThrottle* download_throttle{};
    const std::vector<HttpHeader>* response_headers{};
    CURL* handle{};
    std::uint64_t checkpoint_interval{};
    std::uint64_t durable_offset{};
    DownloadState* download_state{};
    ResponseAcceptance response_acceptance{ResponseAcceptance::pending};
    bool follow_redirects{false};
    std::stop_token stop_token;
    bool throttle_cancelled{false};
};

struct ReadContext {
    std::string_view body;
    std::size_t offset{0};
    const UploadThrottle* upload_throttle{};
    std::stop_token stop_token;
    bool throttle_cancelled{false};
    CallbackFailure failure;
};

struct HeaderContext {
    std::vector<HttpHeader> headers;
    std::size_t total_size{};
    bool size_exceeded{false};
    CallbackFailure failure;
    WriteContext* write_context{};
};

struct ProgressContext {
    const DownloadProgress* callback{};
    std::stop_token stop_token;
    bool cancelled{false};
    bool failed{false};
    std::string error;
};

void record_callback_failure(
    CallbackFailure& failure, std::exception_ptr exception
) noexcept;
[[nodiscard]] std::string_view callback_storage_error(
    CallbackFailureKind failure, CallbackStorage storage
) noexcept;
bool make_download_checkpoint(WriteContext& context);
std::size_t read_request_body(
    char* destination,
    std::size_t size,
    std::size_t count,
    void* user_data
);
std::size_t write_response(
    char* data,
    std::size_t size,
    std::size_t count,
    void* context
);
std::size_t write_header(
    char* data,
    std::size_t size,
    std::size_t count,
    void* context
);
int report_progress(
    void* context,
    curl_off_t download_total,
    curl_off_t downloaded,
    curl_off_t upload_total,
    curl_off_t uploaded
);

}  // namespace onedrive::http::detail
