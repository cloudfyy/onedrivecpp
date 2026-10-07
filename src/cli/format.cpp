#include "cli/format.hpp"

#include <fmt/format.h>

#include <array>
#include <string_view>

namespace onedrive::cli::detail {

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t unit_size = 1024;
    constexpr std::array<std::string_view, 3> units{
        "KiB",
        "MiB",
        "GiB",
    };
    if (bytes < unit_size) {
        return fmt::format("{} B", bytes);
    }

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (true) {
        value /= static_cast<double>(unit_size);
        if (value < static_cast<double>(unit_size) ||
            unit + 1 == units.size()) {
            break;
        }
        ++unit;
    }
    return fmt::format("{:.1f} {}", value, units.at(unit));
}

std::string format_duration(std::uint64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining_seconds = seconds % 60;
    return fmt::format(
        "{:02}:{:02}:{:02}",
        hours,
        minutes,
        remaining_seconds
    );
}

}  // namespace onedrive::cli::detail
