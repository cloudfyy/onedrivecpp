#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/http/transfer_rate_limiter.hpp"
#include "onedrive/util/remote_time.hpp"
#include "util/ascii.hpp"
#include "util/typestate.hpp"
#include "util/uri.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace onedrive::graph {

using namespace client_detail;
using onedrive::util::percent_encode_uri_component;

namespace {

constexpr std::size_t maximum_download_redirects = 5;

struct ContentRange {
    std::uint64_t first;
    std::uint64_t last;
    std::uint64_t total;
};

[[noreturn]] void invalid_content_range(std::string_view header) {
    throw std::runtime_error(
        "Microsoft Graph chunk download returned an invalid "
        "Content-Range header: " + std::string{header}
    );
}

std::uint64_t parse_content_range_number(
    std::string_view value,
    std::string_view header
) {
    std::uint64_t number{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, number);
    if (value.empty() || error != std::errc{} || position != end) {
        invalid_content_range(header);
    }
    return number;
}

ContentRange parse_content_range(std::string_view header) {
    constexpr std::string_view prefix{"bytes "};
    if (!header.starts_with(prefix)) {
        invalid_content_range(header);
    }
    header.remove_prefix(prefix.size());
    const auto dash = header.find('-');
    const auto slash = header.find('/');
    if (dash == std::string_view::npos ||
        slash == std::string_view::npos ||
        dash == 0 || slash <= dash + 1 || slash + 1 >= header.size()) {
        invalid_content_range(header);
    }
    const ContentRange range{
        .first = parse_content_range_number(header.substr(0, dash), header),
        .last = parse_content_range_number(
            header.substr(dash + 1, slash - dash - 1),
            header
        ),
        .total = parse_content_range_number(header.substr(slash + 1), header),
    };
    if (range.first > range.last || range.last >= range.total) {
        invalid_content_range(header);
    }
    return range;
}

void validate_chunk_response_metadata(
    const http::HttpResponse& response,
    std::uint64_t expected_first,
    std::uint64_t expected_last,
    std::uint64_t expected_total
) {
    if (response.status_code != 206) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download expected HTTP 206 but "
                "received HTTP {}",
                response.status_code
            )
        );
    }
    std::optional<std::string_view> header;
    for (const auto& candidate : response.headers) {
        if (!onedrive::util::ascii_iequals(
                candidate.name,
                "Content-Range"
            )) {
            continue;
        }
        if (header.has_value()) {
            throw std::runtime_error(
                "Microsoft Graph chunk download returned multiple "
                "Content-Range headers"
            );
        }
        header = candidate.value;
    }
    if (!header.has_value()) {
        throw std::runtime_error(
            "Microsoft Graph chunk download did not return Content-Range"
        );
    }
    const auto range = parse_content_range(*header);
    if (range.first != expected_first ||
        range.last != expected_last ||
        range.total != expected_total) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download returned Content-Range "
                "'{}'; expected 'bytes {}-{}/{}'",
                *header,
                expected_first,
                expected_last,
                expected_total
            )
        );
    }
}

void validate_chunk_response(
    const http::HttpResponse& response,
    std::uint64_t expected_first,
    std::uint64_t expected_last,
    std::uint64_t expected_total
) {
    validate_chunk_response_metadata(
        response,
        expected_first,
        expected_last,
        expected_total
    );
    const auto expected_bytes = expected_last - expected_first + 1;
    if (response.received_size != expected_bytes) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download wrote {} bytes; expected {}",
                response.received_size,
                expected_bytes
            )
        );
    }
}

void roll_back_chunk(
    const std::filesystem::path& destination,
    std::uint64_t offset
) {
    std::error_code error;
    if (offset == 0) {
        std::filesystem::remove(destination, error);
    } else {
        std::filesystem::resize_file(destination, offset, error);
    }
    if (error) {
        throw std::runtime_error(
            "cannot roll back rejected Microsoft Graph chunk at byte " +
            std::to_string(offset) + ": " + error.message()
        );
    }
}

[[noreturn]] void throw_download_error(
    std::string_view description,
    const http::HttpError& error
) {
    if (error.code == http::HttpErrorCode::cancelled) {
        throw DownloadCancelledError(
            std::string{description} + " was cancelled"
        );
    }
    throw std::runtime_error(
        std::string{description} + " failed: " + error.message
    );
}

bool accept_successful_download_response(
    long status_code,
    std::span<const http::HttpHeader>
) {
    return successful_status(status_code);
}

void require_successful_download(
    const http::HttpResult& response,
    std::string_view description
) {
    if (!response) {
        throw_download_error(description, response.error());
    }
    if (!successful_status(response->status_code)) {
        throw std::runtime_error(
            std::format(
                "{} failed with HTTP {}",
                description,
                response->status_code
            )
        );
    }
}

}  // namespace

void MicrosoftGraphClient::download_file(
    const std::string& remote_id,
    const std::string& expected_etag,
    std::uint64_t expected_size,
    const std::filesystem::path& destination,
    std::uint64_t initial_offset,
    std::stop_token stop_token,
    const DownloadProgress& progress,
    const DownloadCheckpoint& checkpoint,
    const DownloadData& data
) const {
    const auto& download_transport = options_.download_transport;
    const auto& transfer = download_transport.transfer;
    if (remote_id.empty()) {
        throw std::invalid_argument("cannot download a drive item without an ID");
    }
    if (expected_etag.empty() ||
        expected_etag.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument(
            "cannot download a drive item without a valid eTag"
        );
    }
    if (initial_offset > expected_size) {
        throw std::invalid_argument(
            "download resume offset exceeds the expected file size"
        );
    }
    if (stop_token.stop_requested()) {
        throw DownloadCancelledError("Microsoft Graph download was cancelled");
    }
    if (options_.relaxed_download_validation && initial_offset != 0) {
        throw std::invalid_argument(
            "relaxed Graph downloads cannot resume from a partial file"
        );
    }
    if (!options_.relaxed_download_validation &&
        initial_offset != 0 &&
        initial_offset == expected_size) {
        if (progress) {
            progress(expected_size, expected_size);
        }
        if (checkpoint) {
            checkpoint(expected_size);
        }
        return;
    }
    const std::string content_url =
        graph_drive_prefix(options_) + "/items/" +
        percent_encode_uri_component(remote_id) + "/content";

    const auto request_download_url = [&]() {
        const auto redirect = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::get,
                    .url = content_url,
                    .headers = {
                        "Accept: application/octet-stream",
                        "Authorization: Bearer " + access_token(),
                        "If-Match: " + expected_etag,
                    },
                    .body = {},
                    .connect_timeout = transfer.connect_timeout,
                    .operation_timeout = std::chrono::seconds{60},
                    .maximum_response_size = std::size_t{64} * 1024U,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph download redirect",
            stop_token
        );
        if (!redirect) {
            throw_download_error(
                "Microsoft Graph download request",
                redirect.error()
            );
        }
        if (redirect->status_code == 412) {
            throw RemoteItemChangedError(
                "Microsoft Graph drive item changed before download"
            );
        }
        if (redirect->status_code < 300 || redirect->status_code >= 400) {
            throw std::runtime_error(
                std::format(
                    "Microsoft Graph download did not return a redirect "
                    "(HTTP {})",
                    redirect->status_code
                )
            );
        }
        const auto location = header_value(*redirect, "Location");
        if (!location || !location->starts_with("https://")) {
            throw std::runtime_error(
                "Microsoft Graph download returned an invalid HTTPS redirect"
            );
        }
        spdlog::trace(
            "Received HTTPS download redirect for Microsoft Graph drive item "
            "'{}'",
            remote_id
        );
        return *location;
    };
    auto location = request_download_url();
    const auto low_speed_limit =
        transfer.low_speed_limit_bytes_per_second;
    const auto maximum_receive_speed =
        download_transport.maximum_receive_speed_bytes_per_second;
    const http::DownloadThrottle download_throttle =
        download_rate_limiter_ ?
            http::DownloadThrottle{
                [limiter = download_rate_limiter_.get()](
                    std::size_t bytes,
                    const std::stop_token& stop_token
                ) {
                    return limiter->acquire(bytes, stop_token);
                }
            } :
            http::DownloadThrottle{};
    struct DownloadAttempt {
        std::vector<std::string> headers;
        std::uint64_t offset;
    };
    const auto download =
        [&]<typename MakeAttempt>(MakeAttempt&& make_attempt,
            const DownloadProgress& chunk_progress,
            std::string_view description,
            const DownloadCheckpoint& chunk_checkpoint = {},
            const http::DownloadResponseGate& response_gate = {}) {
        const auto perform_download = [&] {
            return perform_with_retries(
                [&] {
                    auto attempt = make_attempt();
                    return transport_->download(
                        http::HttpRequest{
                            .method = http::HttpMethod::get,
                            .url = location,
                            .headers = std::move(attempt.headers),
                            .body = {},
                            .connect_timeout =
                                transfer.connect_timeout,
                            .operation_timeout =
                                transfer.operation_timeout,
                            .low_speed_timeout =
                                transfer.low_speed_timeout,
                            .low_speed_limit_bytes_per_second =
                                low_speed_limit,
                            .maximum_receive_speed_bytes_per_second =
                                maximum_receive_speed,
                            .http_version =
                                transfer.http_version,
                            .ip_version =
                                transfer.ip_version,
                            .maximum_response_size = 0,
                            .download_offset = attempt.offset,
                            .download_checkpoint_interval_bytes =
                                options_.download_checkpoint_interval_bytes,
                            .private_download_permissions =
                                options_.private_download_permissions,
                            .follow_redirects = true,
                            .maximum_redirects =
                                maximum_download_redirects,
                            .download_throttle =
                                download_throttle,
                            .stop_token = stop_token,
                        },
                        destination,
                        chunk_progress,
                        data,
                        chunk_checkpoint,
                        response_gate
                    );
                },
                options_,
                options_.download_maximum_retries,
                sleep_,
                description,
                stop_token,
                TransportErrorRetry::enabled
            );
        };
        auto response = perform_download();
        if (response &&
            stale_download_url_status(response->status_code)) {
            spdlog::warn(
                "{} returned HTTP {}; refreshing the Microsoft Graph download "
                "redirect",
                description,
                response->status_code
            );
            location = request_download_url();
            response = perform_download();
        }
        return response;
    };

    spdlog::debug("Downloading Microsoft Graph drive item '{}'", remote_id);
    if (options_.relaxed_download_validation) {
        auto response = download(
            [] {
                return DownloadAttempt{
                    .headers = {"Accept: application/octet-stream"},
                    .offset = 0,
                };
            },
            progress,
            "Microsoft Graph relaxed file download",
            {},
            accept_successful_download_response
        );
        require_successful_download(
            response,
            "Microsoft Graph relaxed file download"
        );
        spdlog::warn(
            "Downloaded Microsoft Graph drive item '{}' without relying on "
            "remote size or hash metadata",
            remote_id
        );
        return;
    }
    if (initial_offset == 0 &&
        expected_size <= options_.download_chunk_threshold_bytes) {
        auto response = download(
            [] {
                return DownloadAttempt{
                    .headers = {"Accept: application/octet-stream"},
                    .offset = 0,
                };
            },
            progress,
            "Microsoft Graph file download",
            checkpoint,
            accept_successful_download_response
        );
        require_successful_download(
            response,
            "Microsoft Graph file download"
        );
        spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
        return;
    }

    const auto chunk_size = options_.download_chunk_threshold_bytes;
    spdlog::debug(
        "Downloading Microsoft Graph drive item '{}' in {}-byte chunks",
        remote_id,
        chunk_size
    );
    for (std::uint64_t offset = initial_offset; offset < expected_size;) {
        const auto bytes = std::min(chunk_size, expected_size - offset);
        const auto end = offset + bytes - 1;
        std::uint64_t durable_offset = offset;
        std::uint64_t attempt_offset = offset;
        auto response = download(
            [&] {
                attempt_offset = durable_offset;
                return DownloadAttempt{
                    .headers = {
                        "Accept: application/octet-stream",
                        std::format(
                            "Range: bytes={}-{}",
                            attempt_offset,
                            end
                        ),
                    },
                    .offset = attempt_offset,
                };
            },
            progress ?
                DownloadProgress{
                    [&](std::uint64_t downloaded, std::uint64_t) {
                        progress(
                            std::min(
                                attempt_offset + downloaded,
                                expected_size
                            ),
                            expected_size
                        );
                    }
                } :
                DownloadProgress{},
            std::format(
                "Microsoft Graph chunk download (bytes {}-{})",
                offset,
                end
            ),
            [&](std::uint64_t completed) {
                if (completed < durable_offset || completed > end + 1) {
                    throw std::logic_error(
                        "download transport checkpoint is outside the "
                        "requested byte range"
                    );
                }
                durable_offset = completed;
                if (checkpoint) {
                    checkpoint(completed);
                }
            },
            [&](
                long status_code,
                std::span<const http::HttpHeader> headers
            ) {
                try {
                    validate_chunk_response_metadata(
                        http::HttpResponse{
                            .status_code = status_code,
                            .headers = {
                                headers.begin(),
                                headers.end(),
                            },
                            .body = {},
                            .received_size = 0,
                        },
                        attempt_offset,
                        end,
                        expected_size
                    );
                    return true;
                } catch (...) {
                    return false;
                }
            }
        );
        if (!response) {
            throw_download_error(
                "Microsoft Graph chunk download",
                response.error()
            );
        }
        try {
            validate_chunk_response(
                *response,
                attempt_offset,
                end,
                expected_size
            );
        } catch (...) {
            roll_back_chunk(destination, offset);
            throw;
        }
        offset = end + 1;
    }
    spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
}

}  // namespace onedrive::graph
