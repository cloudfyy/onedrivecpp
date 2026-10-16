#pragma once

#include "onedrive/ui/common/file_history.hpp"

#include <cstdint>
#include <string>

namespace onedrive::cli::detail {

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_duration(std::uint64_t seconds);
[[nodiscard]] std::string
format_file_operation(const events::FileOperationEvent& operation);
[[nodiscard]] std::string
format_file_history_status(const events::FileHistory& history);

}  // namespace onedrive::cli::detail
