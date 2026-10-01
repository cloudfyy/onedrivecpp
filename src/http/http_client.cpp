#include "onedrive/http/http_client.hpp"
#include "onedrive/version.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

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
    bool size_exceeded{false};
};

struct HeaderContext {
    std::vector<HttpHeader> headers;
    std::size_t total_size{};
    bool size_exceeded{false};
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

    constexpr std::size_t maximum_header_size = 64U * 1024U;
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

}  // namespace

HttpResult CurlHttpClient::perform(const HttpRequest& request) const {
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
    };
    HeaderContext header_context;
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
        write_context.body.size()
    );
    return HttpResponse{
        .status_code = status_code,
        .headers = std::move(header_context.headers),
        .body = std::move(write_context.body),
    };
}

}  // namespace onedrive::http
