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

struct UploadSessionTransactionFamily;
using UploadSessionTransactionState =
    util::TransactionState<UploadSessionTransactionFamily>;
struct UploadSessionAbsentState final : UploadSessionTransactionState {};
struct UploadSessionSavedState final : UploadSessionTransactionState {};
struct UploadSessionActiveState final : UploadSessionTransactionState {};
struct UploadSessionFinalizedState final : UploadSessionTransactionState {};
struct UploadSessionTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, UploadSessionSavedState> &&
                std::same_as<Next, UploadSessionAbsentState>) ||
               ((std::same_as<Current, UploadSessionAbsentState> ||
                 std::same_as<Current, UploadSessionSavedState> ||
                 std::same_as<Current, UploadSessionActiveState>) &&
                std::same_as<Next, UploadSessionActiveState>) ||
               (std::same_as<Current, UploadSessionActiveState> &&
                std::same_as<Next, UploadSessionFinalizedState>);
    }
};

struct AbsentUploadSessionPayload {};
struct SavedUploadSessionPayload {
    UploadSession session;
};
struct ActiveUploadSessionPayload {
    UploadSession session;
};
struct FinalizedUploadSessionPayload {
    RemoteItem remote;
};

using AbsentUploadSession = util::StateTransaction<
    UploadSessionAbsentState,
    UploadSessionTransactionFamily,
    AbsentUploadSessionPayload>;
using SavedUploadSession = util::StateTransaction<
    UploadSessionSavedState,
    UploadSessionTransactionFamily,
    SavedUploadSessionPayload>;
using ActiveUploadSession = util::StateTransaction<
    UploadSessionActiveState,
    UploadSessionTransactionFamily,
    ActiveUploadSessionPayload>;
using FinalizedUploadSession = util::StateTransaction<
    UploadSessionFinalizedState,
    UploadSessionTransactionFamily,
    FinalizedUploadSessionPayload>;

AbsentUploadSession discard_saved_upload_session(
    SavedUploadSession transaction
) noexcept {
    return util::transition_transaction<UploadSessionAbsentState>(
        std::move(transaction),
        [](SavedUploadSessionPayload&&) noexcept {
            return AbsentUploadSessionPayload{};
        }
    );
}

ActiveUploadSession create_upload_session(
    AbsentUploadSession transaction,
    UploadSession session
) noexcept {
    return util::transition_transaction<UploadSessionActiveState>(
        std::move(transaction),
        [session = std::move(session)](
            AbsentUploadSessionPayload&&
        ) mutable noexcept {
            return ActiveUploadSessionPayload{
                .session = std::move(session),
            };
        }
    );
}

ActiveUploadSession resume_upload_session(
    SavedUploadSession transaction,
    UploadSession session
) noexcept {
    return util::transition_transaction<UploadSessionActiveState>(
        std::move(transaction),
        [session = std::move(session)](
            SavedUploadSessionPayload&&
        ) mutable noexcept {
            return ActiveUploadSessionPayload{
                .session = std::move(session),
            };
        }
    );
}

ActiveUploadSession advance_upload_session(
    ActiveUploadSession transaction,
    UploadSession session
) noexcept {
    return util::transition_transaction<UploadSessionActiveState>(
        std::move(transaction),
        [session = std::move(session)](
            ActiveUploadSessionPayload&&
        ) mutable noexcept {
            return ActiveUploadSessionPayload{
                .session = std::move(session),
            };
        }
    );
}

FinalizedUploadSession finalize_upload_session(
    ActiveUploadSession transaction,
    RemoteItem remote
) noexcept {
    return util::transition_transaction<UploadSessionFinalizedState>(
        std::move(transaction),
        [remote = std::move(remote)](
            ActiveUploadSessionPayload&&
        ) mutable noexcept {
            return FinalizedUploadSessionPayload{
                .remote = std::move(remote),
            };
        }
    );
}

template <typename Transaction>
concept DiscardableSavedUploadSession =
    requires(Transaction transaction) {
        discard_saved_upload_session(std::move(transaction));
    };

template <typename Transaction>
concept CreatableUploadSession = requires(
    Transaction transaction,
    UploadSession session
) {
    create_upload_session(std::move(transaction), std::move(session));
};

template <typename Transaction>
concept ResumableUploadSession = requires(
    Transaction transaction,
    UploadSession session
) {
    resume_upload_session(std::move(transaction), std::move(session));
};

template <typename Transaction>
concept AdvanceableUploadSession = requires(
    Transaction transaction,
    UploadSession session
) {
    advance_upload_session(std::move(transaction), std::move(session));
};

template <typename Transaction>
concept FinalizableUploadSession = requires(
    Transaction transaction,
    RemoteItem remote
) {
    finalize_upload_session(std::move(transaction), std::move(remote));
};

template <typename Transaction>
concept HasUploadSession = requires(Transaction transaction) {
    transaction.session.upload_url;
};

template <typename Transaction>
concept HasFinalizedUpload = requires(Transaction transaction) {
    transaction.remote.id;
};

static_assert(DiscardableSavedUploadSession<SavedUploadSession>);
static_assert(!DiscardableSavedUploadSession<AbsentUploadSession>);
static_assert(CreatableUploadSession<AbsentUploadSession>);
static_assert(!CreatableUploadSession<SavedUploadSession>);
static_assert(ResumableUploadSession<SavedUploadSession>);
static_assert(!ResumableUploadSession<AbsentUploadSession>);
static_assert(AdvanceableUploadSession<ActiveUploadSession>);
static_assert(!AdvanceableUploadSession<SavedUploadSession>);
static_assert(FinalizableUploadSession<ActiveUploadSession>);
static_assert(!FinalizableUploadSession<SavedUploadSession>);
static_assert(!HasUploadSession<AbsentUploadSession>);
static_assert(HasUploadSession<SavedUploadSession>);
static_assert(HasUploadSession<ActiveUploadSession>);
static_assert(!HasUploadSession<FinalizedUploadSession>);
static_assert(!HasFinalizedUpload<AbsentUploadSession>);
static_assert(!HasFinalizedUpload<ActiveUploadSession>);
static_assert(HasFinalizedUpload<FinalizedUploadSession>);

void validate_upload_url(std::string_view url) {
    constexpr std::string_view scheme{"https://"};
    if (!url.starts_with(scheme) ||
        url.find_first_of("\r\n\\#") != std::string_view::npos) {
        throw std::runtime_error(
            "Microsoft Graph returned an unsafe upload session URL"
        );
    }
    if (std::ranges::any_of(url, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte <= 0x20U || byte == 0x7FU;
        })) {
        throw std::runtime_error(
            "Microsoft Graph returned an unsafe upload session URL"
        );
    }
    const auto authority_end = url.find_first_of("/?", scheme.size());
    const auto authority = url.substr(
        scheme.size(),
        authority_end == std::string_view::npos ?
            std::string_view::npos :
            authority_end - scheme.size()
    );
    if (authority.empty() || authority.contains('@')) {
        throw std::runtime_error(
            "Microsoft Graph returned an unsafe upload session URL"
        );
    }
}

std::uint64_t parse_upload_range_number(
    std::string_view value,
    std::string_view range
) {
    std::uint64_t number = 0;
    const auto [position, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        number
    );
    if (value.empty() || error != std::errc{} ||
        position != value.data() + value.size()) {
        throw std::runtime_error(
            "Microsoft Graph returned invalid nextExpectedRanges entry: " +
            std::string{range}
        );
    }
    return number;
}

std::uint64_t parse_upload_range_start(
    std::string_view range,
    std::uint64_t total_size
) {
    const auto separator = range.find('-');
    if (separator == std::string_view::npos ||
        range.find('-', separator + 1) != std::string_view::npos) {
        throw std::runtime_error(
            "Microsoft Graph returned invalid nextExpectedRanges entry: " +
            std::string{range}
        );
    }
    const auto start =
        parse_upload_range_number(range.substr(0, separator), range);
    if (start > total_size) {
        throw std::runtime_error(
            "Microsoft Graph returned an out-of-range upload offset"
        );
    }
    const auto end_text = range.substr(separator + 1);
    if (!end_text.empty()) {
        const auto end = parse_upload_range_number(end_text, range);
        if (end < start || end >= total_size) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid upload range"
            );
        }
    }
    return start;
}

std::uint64_t next_upload_offset(
    const Json& json,
    std::uint64_t total_size
) {
    const auto ranges = json.find("nextExpectedRanges");
    if (ranges == json.end() || !ranges->is_array() || ranges->empty()) {
        throw std::runtime_error(
            "Microsoft Graph upload session response is missing "
            "nextExpectedRanges"
        );
    }
    std::optional<std::uint64_t> first;
    for (const auto& range : *ranges) {
        if (!range.is_string()) {
            throw std::runtime_error(
                "Microsoft Graph upload session returned a non-string range"
            );
        }
        const auto start = parse_upload_range_start(
            range.get_ref<const std::string&>(),
            total_size
        );
        first = first ? std::min(*first, start) : start;
    }
    if (!first) {
        throw std::runtime_error(
            "Microsoft Graph upload session returned no usable range"
        );
    }
    return *first;
}

void require_next_upload_offset(
    const Json& json,
    std::uint64_t expected,
    std::uint64_t total_size
) {
    if (next_upload_offset(json, total_size) != expected) {
        throw std::runtime_error(
            "Microsoft Graph upload session returned a non-contiguous range"
        );
    }
}

}  // namespace

RemoteItem MicrosoftGraphClient::upload_file(
    const std::string& remote_path,
    const std::optional<std::string>& remote_id,
    const std::string& expected_etag,
    const std::filesystem::path& source,
    const std::optional<UploadSession>& saved_session,
    const UploadCheckpoint& checkpoint,
    std::stop_token stop_token
) const {
    if (remote_id.has_value() != !expected_etag.empty() ||
        expected_etag.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument(
            "modified uploads require both a remote ID and a valid eTag"
        );
    }
    const auto size = std::filesystem::file_size(source);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::uint64_t>::max()
               )) {
        throw std::runtime_error(
            "local file is too large for a Microsoft Graph upload: " +
            source.string()
        );
    }

    const auto drive_prefix = graph_drive_prefix(options_);
    const auto& upload_transport = options_.upload_transport;
    const auto& transfer = upload_transport.transfer;
    const auto maximum_send_speed =
        upload_transport.maximum_send_speed_bytes_per_second;
    const http::UploadThrottle upload_throttle =
        upload_rate_limiter_ ?
            http::UploadThrottle{
                [limiter = upload_rate_limiter_.get()](
                    std::size_t bytes,
                    const std::stop_token& stop_token
                ) {
                    return limiter->acquire(bytes, stop_token);
                }
            } :
            http::UploadThrottle{};
    const auto require_upload_success = [&](const http::HttpResponse& response,
                                            const Json& json) {
        if (!successful_status(response.status_code)) {
            if (response.status_code == 409 ||
                response.status_code == 412) {
                throw UploadConflictError(
                    graph_error_message(json, response.status_code)
                );
            }
            throw_upload_response_error(json, response.status_code);
        }
    };

    if (size <= options_.simple_upload_threshold_bytes && !saved_session) {
        if (size > static_cast<std::uintmax_t>(
                       std::numeric_limits<std::size_t>::max()
                   )) {
            throw std::runtime_error(
                "local file is too large for a simple upload: " +
                source.string()
            );
        }
        std::ifstream input{source, std::ios::binary};
        if (!input) {
            throw std::runtime_error(
                "cannot open local upload snapshot: " + source.string()
            );
        }
        std::string body(static_cast<std::size_t>(size), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        if (input.gcount() != static_cast<std::streamsize>(body.size()) ||
            input.peek() != std::ifstream::traits_type::eof()) {
            throw std::runtime_error(
                "cannot read stable local upload snapshot: " +
                source.string()
            );
        }

        const std::string url = remote_id ?
            drive_prefix + "/items/" +
                percent_encode_uri_component(*remote_id) +
                "/content" :
            drive_prefix + "/root:/" +
                percent_encode_remote_path(remote_path) +
                ":/content?@microsoft.graph.conflictBehavior=fail";
        std::vector<std::string> headers{
            "Accept: application/json",
            "Authorization: Bearer " + access_token(),
            "Content-Type: application/octet-stream",
        };
        if (remote_id) {
            headers.push_back("If-Match: " + expected_etag);
        }
        const auto response = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::put,
                    .url = url,
                    .headers = headers,
                    .body = body,
                    .connect_timeout = transfer.connect_timeout,
                    .operation_timeout = transfer.operation_timeout,
                    .low_speed_timeout = transfer.low_speed_timeout,
                    .low_speed_limit_bytes_per_second =
                        transfer.low_speed_limit_bytes_per_second,
                    .maximum_send_speed_bytes_per_second =
                        maximum_send_speed,
                    .http_version = transfer.http_version,
                    .ip_version = transfer.ip_version,
                    .maximum_response_size = std::size_t{1024} * 1024U,
                    .upload_throttle = upload_throttle,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph file upload",
            stop_token
        );
        if (!response) {
            if (stop_token.stop_requested() ||
                response.error().code == http::HttpErrorCode::cancelled) {
                throw RequestCancelledError{
                    "Microsoft Graph upload was cancelled"
                };
            }
            throw std::runtime_error(
                "Microsoft Graph upload failed: " +
                response.error().message
            );
        }
        const auto json = parse_graph_json(*response, "upload");
        require_upload_success(*response, json);
        return parse_drive_item(
            json,
            "upload response",
            options_.relaxed_download_validation ?
                ContentValidation::relaxed :
                ContentValidation::strict
        );
    }

    const auto encoded_path = percent_encode_remote_path(remote_path);
    const auto separator = remote_path.rfind('/');
    const auto name = separator == std::string::npos ?
        remote_path :
        remote_path.substr(separator + 1);
    const auto total_size = static_cast<std::uint64_t>(size);
    const auto session_from_json = [&](const Json& json,
                                       std::uint64_t completed_bytes) {
        UploadSession result;
        try {
            result.upload_url = json.at("uploadUrl").get<std::string>();
            result.expiration =
                json.at("expirationDateTime").get<std::string>();
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph upload session is missing required data: " +
                std::string{error.what()}
            );
        }
        validate_upload_url(result.upload_url);
        static_cast<void>(
            util::parse_remote_modified_time(result.expiration)
        );
        result.completed_bytes = completed_bytes;
        return result;
    };
    const auto create_session = [&](AbsentUploadSession absent) {
        const std::string session_url = remote_id ?
            drive_prefix + "/items/" +
                percent_encode_uri_component(*remote_id) +
                "/createUploadSession" :
            drive_prefix + "/root:/" + encoded_path +
                ":/createUploadSession";
        std::vector<std::string> session_headers{
            "Accept: application/json",
            "Authorization: Bearer " + access_token(),
            "Content-Type: application/json",
        };
        if (remote_id) {
            session_headers.push_back("If-Match: " + expected_etag);
        }
        const std::string session_body = Json{
            {"item", {
                {
                    "@microsoft.graph.conflictBehavior",
                    remote_id ? "replace" : "fail",
                },
                {"name", name},
            }},
        }.dump();
        const auto response = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::post,
                    .url = session_url,
                    .headers = session_headers,
                    .body = session_body,
                    .connect_timeout = transfer.connect_timeout,
                    .operation_timeout = transfer.operation_timeout,
                    .low_speed_timeout = transfer.low_speed_timeout,
                    .low_speed_limit_bytes_per_second =
                        transfer.low_speed_limit_bytes_per_second,
                    .maximum_send_speed_bytes_per_second =
                        maximum_send_speed,
                    .http_version = transfer.http_version,
                    .ip_version = transfer.ip_version,
                    .maximum_response_size = std::size_t{1024} * 1024U,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph upload session creation",
            stop_token
        );
        if (!response) {
            if (stop_token.stop_requested() ||
                response.error().code == http::HttpErrorCode::cancelled) {
                throw RequestCancelledError{
                    "Microsoft Graph upload session creation was cancelled"
                };
            }
            throw std::runtime_error(
                "Microsoft Graph upload session creation failed: " +
                response.error().message
            );
        }
        const auto json = parse_graph_json(*response, "upload session");
        require_upload_success(*response, json);
        require_next_upload_offset(json, 0, total_size);
        auto created = session_from_json(json, 0);
        if (checkpoint) {
            checkpoint(created);
        }
        return create_upload_session(
            std::move(absent),
            std::move(created)
        );
    };

    using ResolvedUploadSession =
        std::variant<AbsentUploadSession, ActiveUploadSession>;
    auto resolved_session = [&]() -> ResolvedUploadSession {
        if (!saved_session) {
            return AbsentUploadSession{AbsentUploadSessionPayload{}};
        }
        auto saved = SavedUploadSession{
            SavedUploadSessionPayload{*saved_session},
        };
        validate_upload_url(saved.session.upload_url);
        const auto expiration =
            util::parse_remote_modified_time(saved.session.expiration);
        if (saved.session.completed_bytes > total_size) {
            throw std::runtime_error(
                "saved upload session offset exceeds the local snapshot"
            );
        }
        if (expiration <= std::chrono::system_clock::now()) {
            return discard_saved_upload_session(std::move(saved));
        }
        const auto response = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::get,
                    .url = saved.session.upload_url,
                    .headers = {"Accept: application/json"},
                    .body = {},
                    .connect_timeout = transfer.connect_timeout,
                    .operation_timeout = transfer.operation_timeout,
                    .low_speed_timeout = transfer.low_speed_timeout,
                    .low_speed_limit_bytes_per_second =
                        transfer.low_speed_limit_bytes_per_second,
                    .maximum_response_size =
                        std::size_t{1024} * 1024U,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph upload session status",
            stop_token,
            TransportErrorRetry::enabled
        );
        if (!response) {
            if (stop_token.stop_requested() ||
                response.error().code == http::HttpErrorCode::cancelled) {
                throw RequestCancelledError{
                    "Microsoft Graph upload session status was cancelled"
                };
            }
            throw std::runtime_error(
                "Microsoft Graph upload session status failed: " +
                response.error().message
            );
        }
        if (response->status_code == 404 ||
            response->status_code == 410) {
            return discard_saved_upload_session(std::move(saved));
        }
        const auto json =
            parse_graph_json(*response, "upload session status");
        require_upload_success(*response, json);
        const auto remote_offset =
            next_upload_offset(json, total_size);
        if (remote_offset < saved.session.completed_bytes) {
            throw std::runtime_error(
                "Microsoft Graph upload session offset moved backward"
            );
        }
        auto resumed = saved.session;
        try {
            resumed.expiration =
                json.at("expirationDateTime").get<std::string>();
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph upload session status is missing "
                "expiration: " + std::string{error.what()}
            );
        }
        static_cast<void>(
            util::parse_remote_modified_time(resumed.expiration)
        );
        resumed.completed_bytes = remote_offset;
        if (checkpoint &&
            (resumed.completed_bytes != saved.session.completed_bytes ||
             resumed.expiration != saved.session.expiration)) {
            checkpoint(resumed);
        }
        return resume_upload_session(
            std::move(saved),
            std::move(resumed)
        );
    }();
    auto active_session = std::visit(
        [&](auto&& transaction) -> ActiveUploadSession {
            using Transaction =
                std::remove_cvref_t<decltype(transaction)>;
            if constexpr (std::same_as<Transaction, ActiveUploadSession>) {
                return std::move(transaction);
            } else {
                return create_session(std::move(transaction));
            }
        },
        std::move(resolved_session)
    );

    std::ifstream input{source, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open local upload snapshot: " + source.string()
        );
    }
    input.seekg(
        static_cast<std::streamoff>(
            active_session.session.completed_bytes
        )
    );
    if (!input) {
        throw std::runtime_error(
            "cannot seek local upload snapshot: " + source.string()
        );
    }
    const auto chunk_size = options_.upload_chunk_size_bytes;
    for (std::uint64_t offset =
             active_session.session.completed_bytes;
         offset < total_size;) {
        const auto bytes = std::min(chunk_size, total_size - offset);
        if (bytes > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max()
                    ) ||
            bytes > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamsize>::max()
                    )) {
            throw std::runtime_error(
                "configured upload chunk does not fit in memory"
            );
        }
        std::string body(static_cast<std::size_t>(bytes), '\0');
        input.read(body.data(), static_cast<std::streamsize>(bytes));
        if (input.gcount() != static_cast<std::streamsize>(bytes)) {
            throw std::runtime_error(
                "cannot read stable local upload snapshot: " +
                source.string()
            );
        }
        const auto end = offset + bytes - 1;
        const std::vector<std::string> chunk_headers{
            "Accept: application/json",
            "Content-Type: application/octet-stream",
            "Content-Length: " + std::to_string(bytes),
            std::format(
                "Content-Range: bytes {}-{}/{}",
                offset,
                end,
                total_size
            ),
        };
        const auto chunk_response = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::put,
                    .url = active_session.session.upload_url,
                    .headers = chunk_headers,
                    .body = body,
                    .connect_timeout = transfer.connect_timeout,
                    .operation_timeout = transfer.operation_timeout,
                    .low_speed_timeout = transfer.low_speed_timeout,
                    .low_speed_limit_bytes_per_second =
                        transfer.low_speed_limit_bytes_per_second,
                    .maximum_send_speed_bytes_per_second =
                        maximum_send_speed,
                    .http_version = transfer.http_version,
                    .ip_version = transfer.ip_version,
                    .maximum_response_size = std::size_t{1024} * 1024U,
                    .upload_throttle = upload_throttle,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph upload session fragment",
            stop_token,
            TransportErrorRetry::enabled
        );
        if (!chunk_response) {
            if (stop_token.stop_requested() ||
                chunk_response.error().code ==
                    http::HttpErrorCode::cancelled) {
                throw RequestCancelledError{
                    "Microsoft Graph upload fragment was cancelled"
                };
            }
            throw std::runtime_error(
                "Microsoft Graph upload fragment failed: " +
                chunk_response.error().message
            );
        }
        const auto chunk_json =
            parse_graph_json(*chunk_response, "upload fragment");
        require_upload_success(*chunk_response, chunk_json);
        const auto next_offset = end + 1;
        if (chunk_response->status_code == 202) {
            if (next_offset >= total_size) {
                throw std::runtime_error(
                    "Microsoft Graph did not finalize the last upload fragment"
                );
            }
            require_next_upload_offset(
                chunk_json,
                next_offset,
                total_size
            );
            auto advanced_session = active_session.session;
            try {
                advanced_session.expiration =
                    chunk_json.at("expirationDateTime").get<std::string>();
            } catch (const Json::exception& error) {
                throw std::runtime_error(
                    "Microsoft Graph upload fragment is missing expiration: " +
                    std::string{error.what()}
                );
            }
            static_cast<void>(
                util::parse_remote_modified_time(
                    advanced_session.expiration
                )
            );
            advanced_session.completed_bytes = next_offset;
            auto advanced = advance_upload_session(
                std::move(active_session),
                std::move(advanced_session)
            );
            if (checkpoint) {
                checkpoint(advanced.session);
            }
            active_session = std::move(advanced);
            offset = next_offset;
            continue;
        }
        if (chunk_response->status_code != 200 &&
            chunk_response->status_code != 201) {
            throw std::runtime_error(
                "Microsoft Graph upload fragment returned an unexpected "
                "success status"
            );
        }
        if (next_offset != total_size) {
            throw std::runtime_error(
                "Microsoft Graph finalized an incomplete upload session"
            );
        }
        auto finalized = finalize_upload_session(
            std::move(active_session),
            parse_drive_item(
                chunk_json,
                "upload session response",
                options_.relaxed_download_validation ?
                    ContentValidation::relaxed :
                    ContentValidation::strict
            )
        );
        return std::move(finalized.remote);
    }
    throw std::runtime_error(
        "Microsoft Graph upload session ended without a final item"
    );
}

}  // namespace onedrive::graph
