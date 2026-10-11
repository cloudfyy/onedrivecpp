#pragma once

namespace onedrive::exit_code {

inline constexpr int success = 0;
inline constexpr int failure = 1;
inline constexpr int usage_error = 2;
inline constexpr int cancelled = 130;

// Synchronization uses 2 for blocked items, not argument errors.
inline constexpr int blocked = 2;

}  // namespace onedrive::exit_code
