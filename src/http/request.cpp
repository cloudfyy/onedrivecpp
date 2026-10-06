#include "http/request.hpp"
#include "http/curl.hpp"

#include "onedrive/version.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace onedrive::http::detail {

struct HeaderListDeleter {
    void operator()(curl_slist* headers) const noexcept {
        curl_slist_free_all(headers);
    }
};

class HeaderList {
public:
    [[nodiscard]] bool append(const char* header) {
        curl_slist* updated = curl_slist_append(headers_.get(), header);
        if (updated == nullptr) {
            return false;
        }

        static_cast<void>(headers_.release());
        headers_.reset(updated);
        return true;
    }

    [[nodiscard]] curl_slist* get() const noexcept {
        return headers_.get();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(headers_);
    }

private:
    std::unique_ptr<curl_slist, HeaderListDeleter> headers_;
};

std::string_view method_name(HttpMethod method) {
    switch (method) {
        case HttpMethod::get:
            return "GET";
        case HttpMethod::post:
            return "POST";
        case HttpMethod::put:
            return "PUT";
        case HttpMethod::patch:
            return "PATCH";
        case HttpMethod::delete_:
            return "DELETE";
    }
    std::unreachable();
}

long curl_http_version(HttpVersion version) {
    switch (version) {
    case HttpVersion::automatic:
        return CURL_HTTP_VERSION_NONE;
    case HttpVersion::http_1_1:
        return CURL_HTTP_VERSION_1_1;
    case HttpVersion::http_2:
        return CURL_HTTP_VERSION_2TLS;
    }
    return CURL_HTTP_VERSION_NONE;
}

long curl_ip_version(IpVersion version) {
    switch (version) {
    case IpVersion::automatic:
        return CURL_IPRESOLVE_WHATEVER;
    case IpVersion::ipv4:
        return CURL_IPRESOLVE_V4;
    case IpVersion::ipv6:
        return CURL_IPRESOLVE_V6;
    }
    return CURL_IPRESOLVE_WHATEVER;
}

bool fits_curl_long(std::uint64_t value) {
    return value <=
           static_cast<std::uint64_t>(std::numeric_limits<long>::max());
}

std::string_view negotiated_http_version(long version) {
    switch (version) {
    case CURL_HTTP_VERSION_1_0:
        return "1.0";
    case CURL_HTTP_VERSION_1_1:
        return "1.1";
    case CURL_HTTP_VERSION_2_0:
        return "2";
    case CURL_HTTP_VERSION_3:
        return "3";
    default:
        return "unknown";
    }
}

curl_off_t elapsed_between(curl_off_t later, curl_off_t earlier) {
    return later >= earlier ? later - earlier : 0;
}

void log_transfer_diagnostics(
    CURL* handle,
    HttpMethod method,
    CURLcode transfer_result
) {
    if (!spdlog::should_log(spdlog::level::trace)) {
        return;
    }

    curl_off_t name_lookup_time = 0;
    curl_off_t connect_time = 0;
    curl_off_t app_connect_time = 0;
    curl_off_t start_transfer_time = 0;
    curl_off_t total_time = 0;
    long http_version = CURL_HTTP_VERSION_NONE;
    long new_connections = 0;
    const std::array results{
        curl_easy_getinfo(
            handle,
            CURLINFO_NAMELOOKUP_TIME_T,
            &name_lookup_time
        ),
        curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME_T, &connect_time),
        curl_easy_getinfo(
            handle,
            CURLINFO_APPCONNECT_TIME_T,
            &app_connect_time
        ),
        curl_easy_getinfo(
            handle,
            CURLINFO_STARTTRANSFER_TIME_T,
            &start_transfer_time
        ),
        curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME_T, &total_time),
        curl_easy_getinfo(handle, CURLINFO_HTTP_VERSION, &http_version),
        curl_easy_getinfo(handle, CURLINFO_NUM_CONNECTS, &new_connections),
    };
    const auto failed = std::ranges::find_if(
        results,
        [](CURLcode result) {
            return result != CURLE_OK;
        }
    );
    if (failed != results.end()) {
        spdlog::debug(
            "Cannot collect HTTP {} transfer diagnostics: {}",
            method_name(method),
            curl_easy_strerror(*failed)
        );
        return;
    }

    const auto connection_ready_time =
        std::max(connect_time, app_connect_time);
    spdlog::trace(
        "HTTP {} transport diagnostics: result='{}', http_version={}, "
        "new_connections={}, dns_us={}, tcp_connect_us={}, "
        "tls_handshake_us={}, server_wait_us={}, transfer_us={}, total_us={}",
        method_name(method),
        curl_easy_strerror(transfer_result),
        negotiated_http_version(http_version),
        new_connections,
        name_lookup_time,
        elapsed_between(connect_time, name_lookup_time),
        app_connect_time == 0 ?
            0 :
            elapsed_between(app_connect_time, connect_time),
        elapsed_between(start_transfer_time, connection_ready_time),
        elapsed_between(total_time, start_transfer_time),
        total_time
    );
}

HttpResult perform_request(
    const HttpRequest& request,
    const ProxyOptions& proxy,
    const std::optional<std::string>& proxy_password,
    const std::optional<std::string>& no_proxy,
    int descriptor,
    const DownloadProgress& progress,
    const DownloadData& data,
    const DownloadCheckpoint& checkpoint,
    const DownloadResponseGate& response_gate,
    DownloadState* download_state
) {
    spdlog::trace("Performing HTTP {} request", method_name(request.method));
    const auto connect_timeout = request.connect_timeout.count();
    const auto operation_timeout = request.operation_timeout.count();
    const auto low_speed_timeout = request.low_speed_timeout.count();
    if (connect_timeout <= 0 || operation_timeout <= 0 ||
        low_speed_timeout < 0 ||
        connect_timeout > std::numeric_limits<long>::max() ||
        operation_timeout > std::numeric_limits<long>::max() ||
        low_speed_timeout > std::numeric_limits<long>::max() ||
        (low_speed_timeout > 0 &&
         request.low_speed_limit_bytes_per_second == 0) ||
        !fits_curl_long(request.low_speed_limit_bytes_per_second) ||
        request.maximum_receive_speed_bytes_per_second >
            static_cast<std::uint64_t>(
                std::numeric_limits<curl_off_t>::max()
            ) ||
        request.maximum_send_speed_bytes_per_second >
            static_cast<std::uint64_t>(
                std::numeric_limits<curl_off_t>::max()
            ) ||
        (request.follow_redirects &&
         (request.maximum_redirects == 0 ||
          request.maximum_redirects >
              static_cast<std::size_t>(
                  std::numeric_limits<long>::max()
              ))) ||
        (!request.follow_redirects && request.maximum_redirects != 0)) {
        return std::unexpected(HttpError{
            .message = "HTTP request contains invalid transport options",
        });
    }
    const CURLcode initialization = initialize_curl();
    if (initialization != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot initialize libcurl: " +
                       std::string{curl_easy_strerror(initialization)},
        });
    }

    const CurlHandleLease handle;
    if (!handle) {
        return std::unexpected(HttpError{.message = "cannot create libcurl handle"});
    }

    DownloadState local_download_state{
        .durable_offset = request.download_offset,
        .file_offset = request.download_offset,
        .response_accepted = true,
        .response_validated = false,
    };
    if (download_state == nullptr) {
        download_state = &local_download_state;
    } else {
        *download_state = local_download_state;
    }
    HeaderContext header_context;
    ReadContext read_context{
        .body = request.body,
        .upload_throttle = &request.upload_throttle,
        .stop_token = request.stop_token,
        .failure = {},
    };
    WriteContext write_context{
        .body = {},
        .maximum_size = request.maximum_response_size,
        .descriptor = descriptor,
        .failure = {},
        .file_offset = request.download_offset,
        .download_data = &data,
        .checkpoint = &checkpoint,
        .response_gate = &response_gate,
        .download_throttle = &request.download_throttle,
        .response_headers = &header_context.headers,
        .handle = handle.get(),
        .checkpoint_interval =
            request.download_checkpoint_interval_bytes,
        .durable_offset = request.download_offset,
        .download_state = download_state,
        .response_acceptance = ResponseAcceptance::pending,
        .follow_redirects = request.follow_redirects,
        .stop_token = request.stop_token,
    };
    header_context.write_context = &write_context;
    ProgressContext progress_context{
        .callback = &progress,
        .stop_token = request.stop_token,
        .cancelled = false,
        .failed = false,
        .error = {},
    };
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    HeaderList headers;
    for (const auto& header : request.headers) {
        if (!headers.append(header.c_str())) {
            return std::unexpected(HttpError{.message = "cannot allocate HTTP headers"});
        }
    }

    const auto set_option = [&handle](CURLoption option, auto value) {
        return curl_easy_setopt(handle.get(), option, value);
    };

    CURLcode result = set_option(CURLOPT_URL, request.url.c_str());
    if (result == CURLE_OK && proxy.url) {
        result = set_option(CURLOPT_PROXY, proxy.url->c_str());
    }
    if (result == CURLE_OK && no_proxy) {
        result = set_option(CURLOPT_NOPROXY, no_proxy->c_str());
    }
    if (result == CURLE_OK && proxy.username) {
        result = set_option(
            CURLOPT_PROXYUSERNAME,
            proxy.username->c_str()
        );
    }
    if (result == CURLE_OK && proxy_password) {
        result = set_option(
            CURLOPT_PROXYPASSWORD,
            proxy_password->c_str()
        );
    }
    if (result == CURLE_OK && proxy.url) {
        result = set_option(
            CURLOPT_PROXYAUTH,
            curl_proxy_auth(proxy.auth)
        );
    }
    if (result == CURLE_OK && proxy.ca_file) {
        result = set_option(
            CURLOPT_PROXY_CAINFO,
            proxy.ca_file->c_str()
        );
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_ERRORBUFFER, error_buffer.data());
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_NOSIGNAL, 1L);
    }
    if (result == CURLE_OK && request.follow_redirects) {
        result = set_option(CURLOPT_FOLLOWLOCATION, 1L);
    }
    if (result == CURLE_OK && request.follow_redirects) {
        result = set_option(
            CURLOPT_MAXREDIRS,
            static_cast<long>(request.maximum_redirects)
        );
    }
    if (result == CURLE_OK && request.follow_redirects) {
        result = set_option(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    }
    if (result == CURLE_OK && request.follow_redirects) {
        result = set_option(CURLOPT_UNRESTRICTED_AUTH, 0L);
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_WRITEFUNCTION, &write_response);
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_WRITEDATA, &write_context);
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_HEADERFUNCTION, &write_header);
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_HEADERDATA, &header_context);
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_CONNECTTIMEOUT,
            static_cast<long>(connect_timeout)
        );
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_TIMEOUT,
            static_cast<long>(operation_timeout)
        );
    }
    if (result == CURLE_OK && low_speed_timeout > 0) {
        result = set_option(
            CURLOPT_LOW_SPEED_TIME,
            static_cast<long>(low_speed_timeout)
        );
    }
    if (result == CURLE_OK && low_speed_timeout > 0) {
        result = set_option(
            CURLOPT_LOW_SPEED_LIMIT,
            static_cast<long>(request.low_speed_limit_bytes_per_second)
        );
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_MAX_RECV_SPEED_LARGE,
            static_cast<curl_off_t>(
                request.maximum_receive_speed_bytes_per_second
            )
        );
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_MAX_SEND_SPEED_LARGE,
            static_cast<curl_off_t>(
                request.maximum_send_speed_bytes_per_second
            )
        );
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_HTTP_VERSION,
            curl_http_version(request.http_version)
        );
    }
    if (result == CURLE_OK) {
        result = set_option(
            CURLOPT_IPRESOLVE,
            curl_ip_version(request.ip_version)
        );
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_USERAGENT, build_info::user_agent);
    }
    const bool monitor_progress =
        progress || request.stop_token.stop_possible();
    if (result == CURLE_OK && monitor_progress) {
        result = set_option(CURLOPT_NOPROGRESS, 0L);
    }
    if (result == CURLE_OK && monitor_progress) {
        result = set_option(CURLOPT_XFERINFOFUNCTION, &report_progress);
    }
    if (result == CURLE_OK && monitor_progress) {
        result = set_option(CURLOPT_XFERINFODATA, &progress_context);
    }
    if (result == CURLE_OK && headers) {
        result = set_option(CURLOPT_HTTPHEADER, headers.get());
    }
    if (result == CURLE_OK && request.method == HttpMethod::post) {
        result = set_option(CURLOPT_POST, 1L);
    }
    if (result == CURLE_OK && request.method == HttpMethod::put) {
        result = set_option(CURLOPT_CUSTOMREQUEST, "PUT");
    }
    if (result == CURLE_OK && request.method == HttpMethod::patch) {
        result = set_option(CURLOPT_CUSTOMREQUEST, "PATCH");
    }
    if (result == CURLE_OK && request.method == HttpMethod::delete_) {
        result = set_option(CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    const bool throttle_upload =
        request.method == HttpMethod::put &&
        static_cast<bool>(request.upload_throttle);
    if (result == CURLE_OK && throttle_upload) {
        result = set_option(CURLOPT_UPLOAD, 1L);
    }
    if (result == CURLE_OK && throttle_upload) {
        result = set_option(CURLOPT_READFUNCTION, &read_request_body);
    }
    if (result == CURLE_OK && throttle_upload) {
        result = set_option(CURLOPT_READDATA, &read_context);
    }
    if (result == CURLE_OK && throttle_upload) {
        result = set_option(
            CURLOPT_INFILESIZE_LARGE,
            static_cast<curl_off_t>(request.body.size())
        );
    }
    if (result == CURLE_OK && request.method != HttpMethod::get &&
        !throttle_upload) {
        result = set_option(CURLOPT_POSTFIELDS, request.body.data());
    }
    if (result == CURLE_OK && request.method != HttpMethod::get &&
        !throttle_upload) {
        result = set_option(
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(request.body.size())
        );
    }
    if (result != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot configure HTTP request: " +
                       std::string{curl_easy_strerror(result)},
        });
    }

    result = curl_easy_perform(handle.get());
    log_transfer_diagnostics(handle.get(), request.method, result);
    if (result != CURLE_OK) {
        if (progress_context.cancelled ||
            write_context.throttle_cancelled ||
            read_context.throttle_cancelled) {
            const bool can_checkpoint_cancelled_data =
                descriptor != -1 &&
                download_state->response_validated &&
                checkpoint &&
                write_context.file_offset > write_context.durable_offset;
            if (can_checkpoint_cancelled_data &&
                !make_download_checkpoint(write_context)) {
                if (write_context.failure.kind ==
                    CallbackFailureKind::write) {
                    return std::unexpected(HttpError{
                        .message = "cannot flush cancelled download: " +
                                   std::string{
                                       std::strerror(
                                           write_context.failure.system_error
                                       )
                                   },
                    });
                }
                return std::unexpected(HttpError{
                    .message =
                        "cancelled download checkpoint callback failed: " +
                        write_context.failure.detail,
                });
            }
            return std::unexpected(HttpError{
                .code = HttpErrorCode::cancelled,
                .message = "HTTP request was cancelled",
            });
        }
        if (progress_context.failed) {
            return std::unexpected(HttpError{
                .message = "download progress callback failed: " +
                           progress_context.error,
            });
        }
        if (header_context.size_exceeded) {
            return std::unexpected(HttpError{
                .message = "HTTP response headers exceeded the configured size limit",
            });
        }
        if (write_context.failure.kind ==
            CallbackFailureKind::response_too_large) {
            return std::unexpected(HttpError{
                .message = "HTTP response exceeded the configured size limit",
            });
        }
        if (write_context.failure.kind == CallbackFailureKind::write) {
            return std::unexpected(HttpError{
                .message = "cannot write HTTP response: " +
                           std::string{std::strerror(
                               write_context.failure.system_error
                           )},
            });
        }
        if (write_context.failure.kind == CallbackFailureKind::throttle) {
            return std::unexpected(HttpError{
                .message = "download throttle failed: " +
                           write_context.failure.detail,
            });
        }
        if (read_context.failure.kind == CallbackFailureKind::throttle) {
            return std::unexpected(HttpError{
                .message = "upload throttle failed: " +
                           read_context.failure.detail,
            });
        }
        if (write_context.failure.kind ==
            CallbackFailureKind::data_callback) {
            return std::unexpected(HttpError{
                .message = "download data callback failed: " +
                           write_context.failure.detail,
            });
        }
        const std::string detail = error_buffer.front() == '\0' ?
                                       curl_easy_strerror(result) :
                                       error_buffer.data();
        const bool redirect_error =
            request.follow_redirects &&
            (result == CURLE_TOO_MANY_REDIRECTS ||
             result == CURLE_UNSUPPORTED_PROTOCOL);
        return std::unexpected(HttpError{
            .code = redirect_error ?
                        HttpErrorCode::redirect :
                        HttpErrorCode::transport,
            .message = "HTTP request failed: " + detail,
        });
    }

    long status_code = 0;
    result = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
    if (result != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot read HTTP status: " +
                       std::string{curl_easy_strerror(result)},
        });
    }

    if (descriptor != -1 &&
        write_context.response_acceptance == ResponseAcceptance::pending &&
        response_gate) {
        try {
            const bool accepted = response_gate(
                status_code,
                header_context.headers
            );
            write_context.response_acceptance =
                accepted ?
                    ResponseAcceptance::accepted :
                    ResponseAcceptance::rejected;
            download_state->response_accepted = accepted;
            download_state->response_validated = accepted;
        } catch (const std::exception& error) {
            return std::unexpected(HttpError{
                .message = "download response gate failed: " +
                           std::string{error.what()},
            });
        } catch (...) {
            return std::unexpected(HttpError{
                .message = "download response gate failed: unknown error",
            });
        }
    }
    if (descriptor != -1 && status_code >= 200 && status_code < 300 &&
        download_state->response_accepted) {
        if (::fsync(descriptor) == -1) {
            return std::unexpected(HttpError{
                .message = "cannot flush downloaded response: " +
                           std::string{std::strerror(errno)},
            });
        }
    }

    spdlog::trace(
        "HTTP {} request completed with status {} and {} response bytes",
        method_name(request.method),
        status_code,
        write_context.received_size
    );
    return HttpResponse{
        .status_code = status_code,
        .headers = std::move(header_context.headers),
        .body = std::move(write_context.body),
        .received_size =
            static_cast<std::uint64_t>(write_context.received_size),
    };
}

}  // namespace onedrive::http::detail
