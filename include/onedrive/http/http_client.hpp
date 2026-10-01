#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
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
    std::size_t maximum_response_size{16U * 1024U * 1024U};
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

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    [[nodiscard]] virtual HttpResult perform(const HttpRequest& request) const = 0;
};

class CurlHttpClient final : public HttpTransport {
public:
    [[nodiscard]] HttpResult perform(const HttpRequest& request) const override;
};

}  // namespace onedrive::http
