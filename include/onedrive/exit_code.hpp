#pragma once

#include <string_view>

namespace onedrive::exit_code {

inline constexpr int success = 0;
inline constexpr int failure = 1;
inline constexpr int usage_error = 2;
inline constexpr int blocked = 3;
inline constexpr int cancelled = 130;

[[nodiscard]] inline constexpr std::string_view sync_result_name(int code) {
    switch (code) {
    case success:
        return "succeeded";
    case failure:
        return "failed";
    case blocked:
        return "blocked";
    case cancelled:
        return "cancelled";
    default:
        return "unknown";
    }
}

}  // namespace onedrive::exit_code
