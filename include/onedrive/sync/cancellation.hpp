#pragma once

#include <stop_token>
#include <stdexcept>

namespace onedrive::sync {

class SyncCancelledError final : public std::runtime_error {
public:
    SyncCancelledError()
        : std::runtime_error{"synchronization cancelled"} {
    }
};

inline void throw_if_cancelled(const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        throw SyncCancelledError{};
    }
}

} // namespace onedrive::sync
