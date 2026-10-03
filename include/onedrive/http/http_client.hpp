#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace onedrive::http {

enum class HttpMethod {
    get,
    post,
};

struct HttpRequest {
    HttpMethod method{HttpMethod::get};
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds operation_timeout{60};
    std::size_t maximum_response_size{
        std::size_t{16} * 1024U * 1024U
    };
    std::uint64_t download_offset{0};
};

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    long status_code{};
    std::vector<HttpHeader> headers;
    std::string body;
};

struct HttpError {
    std::string message;
};

using HttpResult = std::expected<HttpResponse, HttpError>;
using DownloadProgress =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

class HttpTransport {
public:
    HttpTransport() = default;
    virtual ~HttpTransport() = default;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = delete;
    HttpTransport& operator=(HttpTransport&&) = delete;
    [[nodiscard]] virtual HttpResult perform(const HttpRequest& request) const = 0;
    [[nodiscard]] virtual HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const = 0;
};

class CurlHttpClient final : public HttpTransport {
public:
    [[nodiscard]] HttpResult perform(const HttpRequest& request) const override;
    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const override;
};

}  // namespace onedrive::http
