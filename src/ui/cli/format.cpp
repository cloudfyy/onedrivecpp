#include "ui/cli/format.hpp"
#include "onedrive/util/byte_size.hpp"

#include <fmt/format.h>

namespace onedrive::cli::detail {

std::string format_bytes(std::uint64_t bytes) {
    const auto size = util::scale_bytes(bytes, util::ByteUnit::gibibytes);
    return size.unit == "B" ? fmt::format("{} {}", bytes, size.unit)
                            : fmt::format("{:.1f} {}", size.value, size.unit);
}

std::string format_duration(std::uint64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining_seconds = seconds % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, remaining_seconds);
}

} // namespace onedrive::cli::detail
