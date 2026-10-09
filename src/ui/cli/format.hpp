#pragma once

#include <cstdint>
#include <string>

namespace onedrive::cli::detail {

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_duration(std::uint64_t seconds);

}  // namespace onedrive::cli::detail
