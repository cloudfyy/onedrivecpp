#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <proxy/proxy.h>
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

class HttpTransport {
public:
    template <typename Implementation, typename... Args>
    explicit HttpTransport(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              HttpTransportFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit HttpTransport(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit HttpTransport(Implementation& implementation)
        : implementation_{&implementation} {}

    ~HttpTransport() = default;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) noexcept = default;
    HttpTransport& operator=(HttpTransport&&) noexcept = default;

    [[nodiscard]] HttpResult perform(const HttpRequest& request) const {
        return implementation_->perform(request);
    }

    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        return implementation_->download(request, destination, progress);
    }

private:
    pro::proxy<HttpTransportFacade> implementation_;
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
