#pragma once

#include <string>
#include <string_view>
#include <span>
#include <utility>

namespace onedrive::util {

[[nodiscard]] inline std::string percent_encode_uri_component(
    std::string_view value
) {
    constexpr std::string_view hex{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size());
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const bool unreserved =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.' || character == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4U]);
            encoded.push_back(hex[character & 0x0FU]);
        }
    }
    return encoded;
}

[[nodiscard]] inline std::string encode_uri_parameters(
    std::span<const std::pair<std::string_view, std::string_view>> values
) {
    std::string encoded;
    for (const auto& [name, value] : values) {
        if (!encoded.empty()) {
            encoded.push_back('&');
        }
        encoded += percent_encode_uri_component(name);
        encoded.push_back('=');
        encoded += percent_encode_uri_component(value);
    }
    return encoded;
}

}  // namespace onedrive::util
