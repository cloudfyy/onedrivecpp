#pragma once

#include "onedrive/http/http_client.hpp"

#include <deque>
#include <filesystem>
#include <utility>
#include <vector>

namespace onedrive::test {

class QueuedHttpTransport {
public:
    explicit QueuedHttpTransport(std::deque<http::HttpResult> responses)
        : responses_{std::move(responses)} {}

    [[nodiscard]] http::HttpResult perform(
        const http::HttpRequest& request
    ) const {
        requests.push_back(request);
        if (responses_.empty()) {
            return std::unexpected(
                http::HttpError{.message = "no fake response available"}
            );
        }
        auto response = std::move(responses_.front());
        responses_.pop_front();
        return response;
    }

    [[nodiscard]] http::HttpResult download(
        const http::HttpRequest&,
        const std::filesystem::path&,
        const http::DownloadProgress&,
        const http::DownloadData&,
        const http::DownloadCheckpoint&,
        const http::DownloadResponseGate&
    ) const {
        return std::unexpected(
            http::HttpError{.message = "download was not expected"}
        );
    }

    mutable std::vector<http::HttpRequest> requests;

private:
    mutable std::deque<http::HttpResult> responses_;
};

}  // namespace onedrive::test
