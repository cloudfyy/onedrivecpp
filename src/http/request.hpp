#pragma once

#include "http/callbacks.hpp"

#include <optional>
#include <string>

namespace onedrive::http::detail {

[[nodiscard]] HttpResult perform_request(
    const HttpRequest& request,
    const ProxyOptions& proxy,
    const std::optional<std::string>& proxy_password,
    const std::optional<std::string>& no_proxy,
    int descriptor,
    const DownloadProgress& progress = {},
    const DownloadData& data = {},
    const DownloadCheckpoint& checkpoint = {},
    const DownloadResponseGate& response_gate = {},
    DownloadState* download_state = nullptr
);

}  // namespace onedrive::http::detail
