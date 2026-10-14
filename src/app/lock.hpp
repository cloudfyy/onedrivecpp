#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"

#include <filesystem>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace onedrive::events {
class Observer;
}

namespace onedrive::app::detail {

struct LockOwner {
    std::int64_t pid{0};
    std::string operation;
    std::string started_at;
    std::filesystem::path state_directory;
    std::filesystem::path sync_directory;
};

class RuntimeBusyError final : public std::runtime_error {
public:
    RuntimeBusyError(
        std::string resource,
        std::filesystem::path requested_directory,
        LockOwner owner
    );

    const std::string resource;
    const std::filesystem::path requested_directory;
    const LockOwner owner;
};

void report_runtime_busy(
    const RuntimeBusyError& error, const events::Observer& observer
);

[[nodiscard]] onedrive::util::UniqueFD acquire_runtime_lock(
    const std::filesystem::path& state_directory,
    std::string_view operation = "unknown"
);

[[nodiscard]] onedrive::util::UniqueFD acquire_sync_directory_lock(
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory,
    std::string_view operation,
    const events::Observer& observer
);

}  // namespace onedrive::app::detail
