#include "onedrive/http/http_client.hpp"
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

struct HeaderListDeleter {
    void operator()(curl_slist* headers) const noexcept {
        curl_slist_free_all(headers);
    }
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
};

struct HeaderContext {
    std::vector<HttpHeader> headers;
    std::size_t total_size{};
    bool size_exceeded{false};
};

struct ProgressContext {
    const DownloadProgress* callback{};
    bool failed{false};
    std::string error;
};

std::string_view method_name(HttpMethod method) {
    return method == HttpMethod::post ? "POST" : "GET";
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
    try {
        (*progress.callback)(
            downloaded < 0 ? 0U : static_cast<std::uint64_t>(downloaded),
            download_total < 0 ?
                0U :
                static_cast<std::uint64_t>(download_total)
        );
        return 0;
    } catch (const std::exception& error) {
        progress.failed = true;
        progress.error = error.what();
        return 1;
    } catch (...) {
        progress.failed = true;
        progress.error = "unknown error";
        return 1;
    }
}

HttpResult perform_request(
    const HttpRequest& request,
    int descriptor,
    const DownloadProgress& progress = {}
) {
    spdlog::trace("Performing HTTP {} request", method_name(request.method));
    static const CurlRuntime runtime;
    if (runtime.result() != CURLE_OK) {
        return std::unexpected(HttpError{
            .message = "cannot initialize libcurl: " +
                       std::string{curl_easy_strerror(runtime.result())},
        });
    }

    const std::unique_ptr<CURL, CurlHandleDeleter> handle{curl_easy_init()};
    if (!handle) {
        return std::unexpected(HttpError{.message = "cannot create libcurl handle"});
    }

    WriteContext write_context{
        .body = {},
        .maximum_size = request.maximum_response_size,
        .descriptor = descriptor,
        .file_offset = request.download_offset,
    };
    HeaderContext header_context;
    ProgressContext progress_context{
        .callback = &progress,
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
        result = set_option(CURLOPT_CONNECTTIMEOUT, request.connect_timeout.count());
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_TIMEOUT, request.operation_timeout.count());
    }
    if (result == CURLE_OK) {
        result = set_option(CURLOPT_USERAGENT, build_info::user_agent);
    }
    if (result == CURLE_OK && progress) {
        result = set_option(CURLOPT_NOPROGRESS, 0L);
    }
    if (result == CURLE_OK && progress) {
        result = set_option(CURLOPT_XFERINFOFUNCTION, &report_progress);
    }
    if (result == CURLE_OK && progress) {
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
    };
}

}  // namespace

HttpResult CurlHttpClient::perform(const HttpRequest& request) const {
    return perform_request(request, -1);
}

HttpResult CurlHttpClient::download(
    const HttpRequest& request,
    const std::filesystem::path& destination,
    const DownloadProgress& progress
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
    const int descriptor = ::open(
        destination.c_str(),
        flags,
        S_IRUSR | S_IWUSR
    );
    if (descriptor == -1) {
        return std::unexpected(HttpError{
            .message = "cannot create download file '" + destination.string() +
                       "': " + std::strerror(errno),
        });
    }

    auto response = perform_request(request, descriptor, progress);
    std::string close_error;
    std::string truncate_error;
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300) &&
        request.download_offset != 0 &&
        ::ftruncate(
            descriptor,
            static_cast<off_t>(request.download_offset)
        ) == -1) {
        truncate_error = std::strerror(errno);
    }
    if (response && response->status_code >= 200 && response->status_code < 300 &&
        ::fsync(descriptor) == -1) {
        response = std::unexpected(HttpError{
            .message = "cannot flush download file '" + destination.string() +
                       "': " + std::strerror(errno),
        });
    }
    if (::close(descriptor) == -1) {
        close_error = std::strerror(errno);
    }
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300 || !close_error.empty()) &&
        request.download_offset == 0) {
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
