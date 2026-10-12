#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace onedrive::util {

enum class ByteUnit {
    bytes,
    kibibytes,
    mebibytes,
    gibibytes,
    tebibytes,
    pebibytes,
    exbibytes
};

struct ScaledBytes {
    double value;
    std::string_view unit;
};

[[nodiscard]] inline ScaledBytes
scale_bytes(std::uint64_t bytes, ByteUnit maximum_unit = ByteUnit::exbibytes) {
    constexpr std::array<std::string_view, 7> units{
        "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"
    };
    auto value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= 1024 && index < static_cast<std::size_t>(maximum_unit)) {
        value /= 1024;
        ++index;
    }
    return {.value = value, .unit = units.at(index)};
}

} // namespace onedrive::util
