#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <sys/stat.h>

namespace onedrive::util {

void write_file_atomically(
    const std::filesystem::path& destination,
    std::span<const std::byte> contents,
    mode_t mode,
    std::string_view description
);

inline void write_file_atomically(
    const std::filesystem::path& destination,
    std::string_view contents,
    mode_t mode,
    std::string_view description
) {
    write_file_atomically(
        destination,
        std::as_bytes(std::span{contents}),
        mode,
        description
    );
}

}  // namespace onedrive::util
