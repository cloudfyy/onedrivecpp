#include "onedrive/http/http_client.hpp"

#include "onedrive/path_security.hpp"
#include "onedrive/version.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::http {
namespace {

class CurlRuntime {
public:
    CurlRuntime() : result_{curl_global_init(CURL_GLOBAL_DEFAULT)} {}

    ~CurlRuntime() {
        if (result_ == CURLE_OK) {
            curl_global_cleanup();
        }
    }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
    CurlRuntime(CurlRuntime&&) = delete;
    CurlRuntime& operator=(CurlRuntime&&) = delete;

    [[nodiscard]] CURLcode result() const noexcept {
        return result_;
    }

private:
    CURLcode result_;
};

struct CurlHandleDeleter {
    void operator()(CURL* handle) const noexcept {
        curl_easy_cleanup(handle);
    }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

class ThreadCurlHandlePool {
public:
    [[nodiscard]] CurlHandle acquire() {
        auto handle = std::move(available_);
        if (!handle) {
            handle.reset(curl_easy_init());
        }
        if (handle) {
            curl_easy_reset(handle.get());
        }
        return handle;
    }

    void release(CurlHandle handle) noexcept {
        if (!available_) {
            available_ = std::move(handle);
        }
    }

private:
    CurlHandle available_;
};

ThreadCurlHandlePool& thread_curl_handle_pool() {
    thread_local ThreadCurlHandlePool pool;
    return pool;
}

class CurlHandleLease {
public:
    CurlHandleLease()
        : pool_{thread_curl_handle_pool()},
          handle_{pool_.acquire()} {}

    ~CurlHandleLease() {
        pool_.release(std::move(handle_));
    }

    CurlHandleLease(const CurlHandleLease&) = delete;
    CurlHandleLease& operator=(const CurlHandleLease&) = delete;
    CurlHandleLease(CurlHandleLease&&) = delete;
    CurlHandleLease& operator=(CurlHandleLease&&) = delete;

    [[nodiscard]] CURL* get() const noexcept {
        return handle_.get();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle_);
    }

private:
    ThreadCurlHandlePool& pool_;
    CurlHandle handle_;
};

struct HeaderListDeleter {
    void operator()(curl_slist* headers) const noexcept {
        curl_slist_free_all(headers);
    }
};

struct DownloadState {
    std::uint64_t durable_offset{};
    std::uint64_t file_offset{};
    bool response_accepted{true};
    bool response_validated{false};
};

struct WriteContext {
    std::string body;
    std::size_t maximum_size;
    int descriptor{-1};
    bool size_exceeded{false};
    bool write_failed{false};
    int write_error{};
    std::size_t received_size{};
    std::uint64_t file_offset{};
    const DownloadData* download_data{};
    const DownloadCheckpoint* checkpoint{};
    const DownloadResponseGate* response_gate{};
    const std::vector<HttpHeader>* response_headers{};
    CURL* handle{};
    std::uint64_t checkpoint_interval{};
    std::uint64_t durable_offset{};
    DownloadState* download_state{};
    std::optional<bool> response_accepted;
    bool data_callback_failed{false};
    std::string data_callback_error;
};

bool make_download_checkpoint(WriteContext& context) {
    if (::fdatasync(context.descriptor) == -1) {
        context.write_failed = true;
        context.write_error = errno;
        return false;
    }
    if (context.checkpoint != nullptr && *context.checkpoint) {
        try {
            (*context.checkpoint)(context.file_offset);
        } catch (const std::exception& error) {
            context.data_callback_failed = true;
            context.data_callback_error = error.what();
            return false;
        } catch (...) {
            context.data_callback_failed = true;
            context.data_callback_error = "unknown error";
            return false;
        }
    }
    context.durable_offset = context.file_offset;
    context.download_state->durable_offset = context.file_offset;
    return true;
}

struct HeaderContext {
    std::vector<HttpHeader> headers;
    std::size_t total_size{};
    bool size_exceeded{false};
};

struct ProgressContext {
    const DownloadProgress* callback{};
    std::stop_token stop_token;
    bool cancelled{false};
    bool failed{false};
    std::string error;
};

constexpr int curl_progress_continue = 0;
constexpr int curl_progress_abort = 1;

std::string_view method_name(HttpMethod method) {
    return method == HttpMethod::post ? "POST" : "GET";
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

bool fits_curl_long(std::uint64_t value) {
    return value <=
           static_cast<std::uint64_t>(std::numeric_limits<long>::max());
}

std::size_t write_response(char* data, std::size_t size, std::size_t count, void* context) {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0;
    }

    const std::size_t byte_count = size * count;
    auto& write_context = *static_cast<WriteContext*>(context);
    if (byte_count >
        std::numeric_limits<std::size_t>::max() - write_context.received_size) {
        return 0;
    }
    write_context.received_size += byte_count;
    if (write_context.descriptor != -1) {
        if (!write_context.response_accepted.has_value()) {
            bool accepted = true;
            if (write_context.response_gate != nullptr &&
                *write_context.response_gate) {
                long status_code = 0;
                if (curl_easy_getinfo(
                        write_context.handle,
                        CURLINFO_RESPONSE_CODE,
                        &status_code
                    ) != CURLE_OK) {
                    write_context.data_callback_failed = true;
                    write_context.data_callback_error =
                        "cannot inspect download response status";
                    return 0;
                }
                try {
                    accepted = (*write_context.response_gate)(
                        status_code,
                        *write_context.response_headers
                    );
                } catch (const std::exception& error) {
                    write_context.data_callback_failed = true;
                    write_context.data_callback_error = error.what();
                    return 0;
                } catch (...) {
                    write_context.data_callback_failed = true;
                    write_context.data_callback_error = "unknown error";
                    return 0;
                }
            }
            write_context.response_accepted = accepted;
            write_context.download_state->response_accepted = accepted;
            write_context.download_state->response_validated =
                write_context.response_gate != nullptr &&
                *write_context.response_gate && accepted;
        }
        if (!*write_context.response_accepted) {
            return byte_count;
        }
        const auto block_offset = write_context.file_offset;
        std::size_t written = 0;
        while (written < byte_count) {
            const auto position =
                write_context.file_offset + static_cast<std::uint64_t>(written);
            if (position >
                static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                write_context.write_failed = true;
                write_context.write_error = EFBIG;
                return 0;
            }
            const auto result = ::pwrite(
                write_context.descriptor,
                data + written,
                byte_count - written,
                static_cast<off_t>(position)
            );
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                write_context.write_failed = true;
                write_context.write_error = errno;
                return 0;
            }
            written += static_cast<std::size_t>(result);
        }
        write_context.file_offset += static_cast<std::uint64_t>(byte_count);
        write_context.download_state->file_offset =
            write_context.file_offset;
        if (write_context.download_data != nullptr &&
            *write_context.download_data) {
            try {
                (*write_context.download_data)(
                    block_offset,
                    std::as_bytes(std::span{data, byte_count})
                );
            } catch (const std::exception& error) {
                write_context.data_callback_failed = true;
                write_context.data_callback_error = error.what();
                return 0;
            } catch (...) {
                write_context.data_callback_failed = true;
                write_context.data_callback_error = "unknown error";
                return 0;
            }
        }
        if (write_context.checkpoint_interval != 0 &&
            write_context.file_offset - write_context.durable_offset >=
                write_context.checkpoint_interval) {
            if (!make_download_checkpoint(write_context)) {
                return 0;
            }
        }
        return byte_count;
    }
    if (byte_count > write_context.maximum_size - write_context.body.size()) {
        write_context.size_exceeded = true;
        return 0;
    }
    try {
        write_context.body.append(data, byte_count);
        return byte_count;
    } catch (...) {
        return 0;
    }
}

std::string_view trim_header_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
            value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

std::size_t write_header(char* data, std::size_t size, std::size_t count, void* context) {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0;
    }

    constexpr std::size_t maximum_header_size =
        std::size_t{64} * 1024U;
    const std::size_t byte_count = size * count;
    auto& header_context = *static_cast<HeaderContext*>(context);
    if (byte_count > maximum_header_size - header_context.total_size) {
        header_context.size_exceeded = true;
        return 0;
    }
    header_context.total_size += byte_count;

    const std::string_view line{data, byte_count};
    const auto separator = line.find(':');
    if (separator == std::string_view::npos) {
        return byte_count;
    }

    try {
        const auto name = trim_header_value(line.substr(0, separator));
        const auto value = trim_header_value(line.substr(separator + 1));
        if (!name.empty()) {
            header_context.headers.push_back({
                .name = std::string{name},
                .value = std::string{value},
            });
        }
        return byte_count;
    } catch (...) {
        return 0;
    }
}

int report_progress(
    void* context,
    curl_off_t download_total,
    curl_off_t downloaded,
    curl_off_t,
    curl_off_t
) {
    auto& progress = *static_cast<ProgressContext*>(context);
    if (progress.stop_token.stop_requested()) {
        progress.cancelled = true;
        return curl_progress_abort;
    }
    if (progress.callback == nullptr || !*progress.callback) {
        return curl_progress_continue;
    }
    try {
        (*progress.callback)(
            downloaded < 0 ? 0U : static_cast<std::uint64_t>(downloaded),
            download_total < 0 ?
                0U :
                static_cast<std::uint64_t>(download_total)
        );
        return curl_progress_continue;
    } catch (const std::exception& error) {
        progress.failed = true;
        progress.error = error.what();
        return curl_progress_abort;
    } catch (...) {
        progress.failed = true;
        progress.error = "unknown error";
        return curl_progress_abort;
    }
}

HttpResult perform_request(
    const HttpRequest& request,
    int descriptor,
    const DownloadProgress& progress = {},
    const DownloadData& data = {},
    const DownloadCheckpoint& checkpoint = {},
    const DownloadResponseGate& response_gate = {},
    DownloadState* download_state = nullptr
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
            )) {
        return std::unexpected(HttpError{
            .message = "HTTP request contains invalid transport options",
        });
    }
    static const CurlRuntime runtime;
    if (runtime.result() != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot initialize libcurl: " +
                       std::string{curl_easy_strerror(runtime.result())},
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
    WriteContext write_context{
        .body = {},
        .maximum_size = request.maximum_response_size,
        .descriptor = descriptor,
        .file_offset = request.download_offset,
        .download_data = &data,
        .checkpoint = &checkpoint,
        .response_gate = &response_gate,
        .response_headers = &header_context.headers,
        .handle = handle.get(),
        .checkpoint_interval =
            request.download_checkpoint_interval_bytes,
        .durable_offset = request.download_offset,
        .download_state = download_state,
        .response_accepted = std::nullopt,
        .data_callback_error = {},
    };
    ProgressContext progress_context{
        .callback = &progress,
        .stop_token = request.stop_token,
        .cancelled = false,
        .failed = false,
        .error = {},
    };
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    std::unique_ptr<curl_slist, HeaderListDeleter> headers;
    curl_slist* header_list = nullptr;
    for (const auto& header : request.headers) {
        curl_slist* updated = curl_slist_append(header_list, header.c_str());
        if (updated == nullptr) {
            curl_slist_free_all(header_list);
            return std::unexpected(HttpError{.message = "cannot allocate HTTP headers"});
        }
        header_list = updated;
    }
    headers.reset(header_list);

    const auto set_option = [&handle](CURLoption option, auto value) {
        return curl_easy_setopt(handle.get(), option, value);
    };

    CURLcode result = set_option(CURLOPT_URL, request.url.c_str());
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_ERRORBUFFER, error_buffer.data());
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
            CURLOPT_HTTP_VERSION,
            curl_http_version(request.http_version)
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
    if (result == CURLE_OK && request.method == HttpMethod::post) {
        result = set_option(CURLOPT_POSTFIELDS, request.body.data());
    }
    if (result == CURLE_OK && request.method == HttpMethod::post) {
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
    if (result != CURLE_OK) {
        if (progress_context.cancelled) {
            const bool can_checkpoint_cancelled_data =
                descriptor != -1 &&
                download_state->response_validated &&
                checkpoint &&
                write_context.file_offset > write_context.durable_offset;
            if (can_checkpoint_cancelled_data &&
                !make_download_checkpoint(write_context)) {
                if (write_context.write_failed) {
                    return std::unexpected(HttpError{
                        .message = "cannot flush cancelled download: " +
                                   std::string{
                                       std::strerror(
                                           write_context.write_error
                                       )
                                   },
                    });
                }
                return std::unexpected(HttpError{
                    .message =
                        "cancelled download checkpoint callback failed: " +
                        write_context.data_callback_error,
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
        if (write_context.size_exceeded) {
            return std::unexpected(HttpError{
                .message = "HTTP response exceeded the configured size limit",
            });
        }
        if (write_context.write_failed) {
            return std::unexpected(HttpError{
                .message = "cannot write HTTP response: " +
                           std::string{std::strerror(write_context.write_error)},
            });
        }
        if (write_context.data_callback_failed) {
            return std::unexpected(HttpError{
                .message = "download data callback failed: " +
                           write_context.data_callback_error,
            });
        }
        const std::string detail = error_buffer.front() == '\0' ?
                                       curl_easy_strerror(result) :
                                       error_buffer.data();
        return std::unexpected(HttpError{.message = "HTTP request failed: " + detail});
    }

    long status_code = 0;
    result = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
    if (result != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot read HTTP status: " +
                       std::string{curl_easy_strerror(result)},
        });
    }

    if (descriptor != -1 && !write_context.response_accepted.has_value() &&
        response_gate) {
        try {
            write_context.response_accepted = response_gate(
                status_code,
                header_context.headers
            );
            download_state->response_accepted =
                *write_context.response_accepted;
            download_state->response_validated =
                *write_context.response_accepted;
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

}  // namespace

HttpResult CurlHttpClient::perform(const HttpRequest& request) const {
    return perform_request(request, -1);
}

HttpResult CurlHttpClient::download(
    const HttpRequest& request,
    const std::filesystem::path& destination,
    const DownloadProgress& progress,
    const DownloadData& data,
    const DownloadCheckpoint& checkpoint,
    const DownloadResponseGate& response_gate
) const {
    if (request.download_offset >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected(HttpError{
            .message = "download offset exceeds the supported file size",
        });
    }
    const int flags = request.download_offset == 0 ?
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW :
                          O_WRONLY | O_CLOEXEC | O_NOFOLLOW;
    const mode_t mode = request.private_download_permissions ?
                            S_IRUSR | S_IWUSR :
                            S_IRUSR | S_IWUSR |
                                S_IRGRP | S_IWGRP |
                                S_IROTH | S_IWOTH;
    int descriptor = -1;
    try {
        descriptor = onedrive::detail::open_path_no_symlinks(
            destination,
            flags,
            request.download_offset == 0 ? mode : 0
        );
    } catch (const std::runtime_error& error) {
        return std::unexpected(HttpError{.message = error.what()});
    }

    DownloadState download_state;
    auto response = perform_request(
        request,
        descriptor,
        progress,
        data,
        checkpoint,
        response_gate,
        &download_state
    );
    std::string close_error;
    std::string truncate_error;
    const bool accepted = download_state.response_accepted;
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300 || !accepted) &&
        download_state.durable_offset != 0 &&
        ::ftruncate(
            descriptor,
            static_cast<off_t>(download_state.durable_offset)
        ) == -1) {
        truncate_error = std::strerror(errno);
    }
    if (::close(descriptor) == -1) {
        close_error = std::strerror(errno);
    }
    bool checkpoint_failed = false;
    if (response && response->status_code >= 200 &&
        response->status_code < 300 && accepted && close_error.empty() &&
        checkpoint &&
        download_state.file_offset > download_state.durable_offset) {
        try {
            checkpoint(download_state.file_offset);
            download_state.durable_offset = download_state.file_offset;
        } catch (const std::exception& error) {
            checkpoint_failed = true;
            response = std::unexpected(HttpError{
                .message = "download checkpoint callback failed: " +
                           std::string{error.what()},
            });
        } catch (...) {
            checkpoint_failed = true;
            response = std::unexpected(HttpError{
                .message =
                    "download checkpoint callback failed: unknown error",
            });
        }
    }
    if ((checkpoint_failed || !close_error.empty()) &&
        download_state.durable_offset != 0) {
        std::error_code error;
        std::filesystem::resize_file(
            destination,
            download_state.durable_offset,
            error
        );
        if (error) {
            truncate_error = error.message();
        }
    }
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300 || !accepted ||
         !close_error.empty()) &&
        download_state.durable_offset == 0) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
    }
    if (!truncate_error.empty()) {
        return std::unexpected(HttpError{
            .message = "cannot roll back partial download file '" +
                       destination.string() + "': " + truncate_error,
        });
    }
    if (!close_error.empty()) {
        return std::unexpected(HttpError{
            .message = "cannot close download file '" + destination.string() +
                       "': " + close_error,
        });
    }
    return response;
}

}  // namespace onedrive::http
