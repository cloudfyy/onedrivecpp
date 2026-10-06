#include "http/callbacks.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <unistd.h>

namespace onedrive::http::detail {

constexpr int curl_progress_continue = 0;
constexpr int curl_progress_abort = 1;

std::size_t read_request_body(
    char* destination,
    std::size_t size,
    std::size_t count,
    void* user_data
) {
    auto& context = *static_cast<ReadContext*>(user_data);
    if (size != 0 &&
        count > std::numeric_limits<std::size_t>::max() / size) {
        context.failure = {
            .kind = CallbackFailureKind::throttle,
            .detail = "upload read buffer size overflow",
        };
        return CURL_READFUNC_ABORT;
    }
    const auto capacity = size * count;
    const auto remaining = context.body.size() - context.offset;
    const auto bytes = std::min(capacity, remaining);
    if (bytes == 0) {
        return 0;
    }
    if (context.upload_throttle != nullptr &&
        *context.upload_throttle) {
        try {
            if (!(*context.upload_throttle)(
                    bytes,
                    context.stop_token
                )) {
                context.throttle_cancelled = true;
                return CURL_READFUNC_ABORT;
            }
        } catch (const std::exception& error) {
            context.failure = {
                .kind = CallbackFailureKind::throttle,
                .detail = error.what(),
            };
            return CURL_READFUNC_ABORT;
        } catch (...) {
            context.failure = {
                .kind = CallbackFailureKind::throttle,
                .detail = "unknown error",
            };
            return CURL_READFUNC_ABORT;
        }
    }
    std::memcpy(
        destination,
        context.body.data() + context.offset,
        bytes
    );
    context.offset += bytes;
    return bytes;
}

bool make_download_checkpoint(WriteContext& context) {
    if (::fdatasync(context.descriptor) == -1) {
        context.failure = {
            .kind = CallbackFailureKind::write,
            .system_error = errno,
            .detail = {},
        };
        return false;
    }
    if (context.checkpoint != nullptr && *context.checkpoint) {
        try {
            (*context.checkpoint)(context.file_offset);
        } catch (const std::exception& error) {
            context.failure = {
                .kind = CallbackFailureKind::data_callback,
                .detail = error.what(),
            };
            return false;
        } catch (...) {
            context.failure = {
                .kind = CallbackFailureKind::data_callback,
                .detail = "unknown error",
            };
            return false;
        }
    }
    context.durable_offset = context.file_offset;
    context.download_state->durable_offset = context.file_offset;
    return true;
}

std::size_t write_response(char* data, std::size_t size, std::size_t count, void* context) {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0;
    }

    const std::size_t byte_count = size * count;
    auto& write_context = *static_cast<WriteContext*>(context);
    if (byte_count >
        std::numeric_limits<std::size_t>::max() - write_context.received_size) {
        return 0;
    }
    write_context.received_size += byte_count;
    if (write_context.descriptor != -1) {
        if (write_context.response_acceptance ==
            ResponseAcceptance::pending) {
            bool accepted = true;
            long status_code = 0;
            if (curl_easy_getinfo(
                    write_context.handle,
                    CURLINFO_RESPONSE_CODE,
                    &status_code
                ) != CURLE_OK) {
                write_context.failure = {
                    .kind = CallbackFailureKind::data_callback,
                    .detail = "cannot inspect download response status",
                };
                return 0;
            }
            if (write_context.follow_redirects &&
                status_code >= 300 && status_code < 400) {
                accepted = false;
            } else if (write_context.response_gate != nullptr &&
                *write_context.response_gate) {
                try {
                    accepted = (*write_context.response_gate)(
                        status_code,
                        *write_context.response_headers
                    );
                } catch (const std::exception& error) {
                    write_context.failure = {
                        .kind = CallbackFailureKind::data_callback,
                        .detail = error.what(),
                    };
                    return 0;
                } catch (...) {
                    write_context.failure = {
                        .kind = CallbackFailureKind::data_callback,
                        .detail = "unknown error",
                    };
                    return 0;
                }
            }
            write_context.response_acceptance =
                accepted ?
                    ResponseAcceptance::accepted :
                    ResponseAcceptance::rejected;
            write_context.download_state->response_accepted = accepted;
            write_context.download_state->response_validated =
                write_context.response_gate != nullptr &&
                *write_context.response_gate && accepted;
        }
        if (write_context.response_acceptance ==
            ResponseAcceptance::rejected) {
            return byte_count;
        }
        if (write_context.download_throttle != nullptr &&
            *write_context.download_throttle) {
            try {
                if (!(*write_context.download_throttle)(
                        byte_count,
                        write_context.stop_token
                    )) {
                    write_context.throttle_cancelled = true;
                    return 0;
                }
            } catch (const std::exception& error) {
                    write_context.failure = {
                        .kind = CallbackFailureKind::throttle,
                        .detail = error.what(),
                    };
                    return 0;
            } catch (...) {
                    write_context.failure = {
                        .kind = CallbackFailureKind::throttle,
                        .detail = "unknown error",
                    };
                    return 0;
            }
        }
        const auto block_offset = write_context.file_offset;
        std::size_t written = 0;
        while (written < byte_count) {
            const auto position =
                write_context.file_offset + static_cast<std::uint64_t>(written);
            if (position >
                static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                write_context.failure = {
                    .kind = CallbackFailureKind::write,
                    .system_error = EFBIG,
                    .detail = {},
                };
                return 0;
            }
            const auto result = ::pwrite(
                write_context.descriptor,
                data + written,
                byte_count - written,
                static_cast<off_t>(position)
            );
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                write_context.failure = {
                    .kind = CallbackFailureKind::write,
                    .system_error = errno,
                    .detail = {},
                };
                return 0;
            }
            written += static_cast<std::size_t>(result);
        }
        write_context.file_offset += static_cast<std::uint64_t>(byte_count);
        write_context.download_state->file_offset =
            write_context.file_offset;
        if (write_context.download_data != nullptr &&
            *write_context.download_data) {
            try {
                (*write_context.download_data)(
                    block_offset,
                    std::as_bytes(std::span{data, byte_count})
                );
            } catch (const std::exception& error) {
                write_context.failure = {
                    .kind = CallbackFailureKind::data_callback,
                    .detail = error.what(),
                };
                return 0;
            } catch (...) {
                write_context.failure = {
                    .kind = CallbackFailureKind::data_callback,
                    .detail = "unknown error",
                };
                return 0;
            }
        }
        if (write_context.checkpoint_interval != 0 &&
            write_context.file_offset - write_context.durable_offset >=
                write_context.checkpoint_interval) {
            if (!make_download_checkpoint(write_context)) {
                return 0;
            }
        }
        return byte_count;
    }
    if (byte_count > write_context.maximum_size - write_context.body.size()) {
        write_context.failure = {
            .kind = CallbackFailureKind::response_too_large,
            .detail = {},
        };
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

    constexpr std::size_t maximum_header_size =
        std::size_t{64} * 1024U;
    const std::size_t byte_count = size * count;
    auto& header_context = *static_cast<HeaderContext*>(context);
    if (byte_count > maximum_header_size - header_context.total_size) {
        header_context.size_exceeded = true;
        return 0;
    }
    header_context.total_size += byte_count;

    const std::string_view line{data, byte_count};
    if (line.starts_with("HTTP/")) {
        header_context.headers.clear();
        if (header_context.write_context != nullptr) {
            auto& write_context = *header_context.write_context;
            write_context.body.clear();
            write_context.received_size = 0;
            write_context.failure = {};
            write_context.response_acceptance =
                ResponseAcceptance::pending;
            write_context.download_state->response_accepted = true;
            write_context.download_state->response_validated = false;
        }
        return byte_count;
    }
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

int report_progress(
    void* context,
    curl_off_t download_total,
    curl_off_t downloaded,
    curl_off_t,
    curl_off_t
) {
    auto& progress = *static_cast<ProgressContext*>(context);
    if (progress.stop_token.stop_requested()) {
        progress.cancelled = true;
        return curl_progress_abort;
    }
    if (progress.callback == nullptr || !*progress.callback) {
        return curl_progress_continue;
    }
    try {
        (*progress.callback)(
            downloaded < 0 ? 0U : static_cast<std::uint64_t>(downloaded),
            download_total < 0 ?
                0U :
                static_cast<std::uint64_t>(download_total)
        );
        return curl_progress_continue;
    } catch (const std::exception& error) {
        progress.failed = true;
        progress.error = error.what();
        return curl_progress_abort;
    } catch (...) {
        progress.failed = true;
        progress.error = "unknown error";
        return curl_progress_abort;
    }
}

}  // namespace onedrive::http::detail
