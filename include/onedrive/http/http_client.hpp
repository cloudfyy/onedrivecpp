#pragma once

#include "onedrive/proxy_service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <proxy/proxy.h>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
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
    std::stop_token stop_token;
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

enum class HttpErrorCode {
    transport,
    cancelled,
};

struct HttpError {
    HttpErrorCode code{HttpErrorCode::transport};
    std::string message;
};

using HttpResult = std::expected<HttpResponse, HttpError>;
using DownloadProgress =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

PRO_DEF_MEM_DISPATCH(HttpPerformDispatch, perform);
PRO_DEF_MEM_DISPATCH(HttpDownloadDispatch, download);

struct HttpTransportFacade : pro::facade_builder
    ::add_convention<
        HttpPerformDispatch,
        HttpResult(const HttpRequest&) const
    >
    ::add_convention<
        HttpDownloadDispatch,
        HttpResult(
            const HttpRequest&,
            const std::filesystem::path&,
            const DownloadProgress&
        ) const
    >
    ::build {};

class HttpTransport : private detail::ProxyService<HttpTransportFacade> {
    using Base = detail::ProxyService<HttpTransportFacade>;

public:
    using Base::Base;

    [[nodiscard]] HttpResult perform(const HttpRequest& request) const {
        return implementation()->perform(request);
    }

    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        return implementation()->download(request, destination, progress);
    }
};

class CurlHttpClient final {
public:
    [[nodiscard]] HttpResult perform(const HttpRequest& request) const;
    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const;
};

}  // namespace onedrive::http
