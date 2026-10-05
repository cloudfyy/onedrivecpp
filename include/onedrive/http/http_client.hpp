#pragma once

#include "onedrive/http/http_options.hpp"
#include "onedrive/util/proxy_service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <proxy/proxy.h>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace onedrive::http {

struct HttpHeader;

using DownloadData = std::function<void(
    std::uint64_t offset,
    std::span<const std::byte> data
)>;
using DownloadCheckpoint =
    std::function<void(std::uint64_t completed_bytes)>;
using DownloadResponseGate = std::function<bool(
    long status_code,
    std::span<const HttpHeader> headers
)>;
using DownloadThrottle = std::function<bool(
    std::size_t bytes,
    const std::stop_token& stop_token
)>;

enum class HttpMethod {
    get,
    post,
    put,
    patch,
    delete_,
};

struct HttpRequest {
    HttpMethod method{HttpMethod::get};
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds operation_timeout{60};
    std::chrono::seconds low_speed_timeout{0};
    std::uint64_t low_speed_limit_bytes_per_second{0};
    std::uint64_t maximum_receive_speed_bytes_per_second{0};
    std::uint64_t maximum_send_speed_bytes_per_second{0};
    HttpVersion http_version{HttpVersion::automatic};
    IpVersion ip_version{IpVersion::automatic};
    std::size_t maximum_response_size{
        std::size_t{16} * 1024U * 1024U
    };
    std::uint64_t download_offset{0};
    std::uint64_t download_checkpoint_interval_bytes{0};
    bool private_download_permissions{true};
    bool follow_redirects{false};
    std::size_t maximum_redirects{0};
    DownloadThrottle download_throttle{};
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
    std::uint64_t received_size{};
};

enum class HttpErrorCode {
    transport,
    redirect,
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
            const DownloadProgress&,
            const DownloadData&,
            const DownloadCheckpoint&,
            const DownloadResponseGate&
        ) const
    >
    ::build {};

class HttpTransport : private onedrive::util::ProxyService<HttpTransportFacade> {
    using Base = onedrive::util::ProxyService<HttpTransportFacade>;

public:
    using Base::Base;

    [[nodiscard]] HttpResult perform(const HttpRequest& request) const {
        return implementation()->perform(request);
    }

    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {},
        const DownloadData& data = {},
        const DownloadCheckpoint& checkpoint = {},
        const DownloadResponseGate& response_gate = {}
    ) const {
        return implementation()->download(
            request,
            destination,
            progress,
            data,
            checkpoint,
            response_gate
        );
    }
};

class CurlHttpClient final {
public:
    explicit CurlHttpClient(ProxyOptions proxy = {});

    [[nodiscard]] HttpResult perform(const HttpRequest& request) const;
    [[nodiscard]] HttpResult download(
        const HttpRequest& request,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {},
        const DownloadData& data = {},
        const DownloadCheckpoint& checkpoint = {},
        const DownloadResponseGate& response_gate = {}
    ) const;

private:
    ProxyOptions proxy_;
    std::optional<std::string> proxy_password_;
    std::optional<std::string> no_proxy_;
};

}  // namespace onedrive::http
