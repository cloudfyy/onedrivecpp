#pragma once

namespace onedrive::http::status {

inline constexpr long ok = 200;
inline constexpr long created = 201;
inline constexpr long accepted = 202;
inline constexpr long partial_content = 206;
inline constexpr long multiple_choices = 300;
inline constexpr long bad_request = 400;
inline constexpr long unauthorized = 401;
inline constexpr long forbidden = 403;
inline constexpr long not_found = 404;
inline constexpr long request_timeout = 408;
inline constexpr long conflict = 409;
inline constexpr long gone = 410;
inline constexpr long precondition_failed = 412;
inline constexpr long too_many_requests = 429;
inline constexpr long bad_gateway = 502;
inline constexpr long service_unavailable = 503;
inline constexpr long gateway_timeout = 504;
inline constexpr long insufficient_storage = 507;

[[nodiscard]] constexpr bool is_success(long code) noexcept {
    return code >= ok && code < multiple_choices;
}

[[nodiscard]] constexpr bool is_redirect(long code) noexcept {
    return code >= multiple_choices && code < bad_request;
}

} // namespace onedrive::http::status
