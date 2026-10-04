#pragma once

#include <string_view>

namespace onedrive::detail {

[[nodiscard]] constexpr bool ascii_iequals(
    std::string_view left,
    std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](unsigned char character) {
            return character >= 'A' && character <= 'Z' ?
                       static_cast<unsigned char>(
                           character - 'A' + 'a'
                       ) :
                       character;
        };
        if (lower(static_cast<unsigned char>(left[index])) !=
            lower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::string_view trim_ascii_whitespace(
    std::string_view value
) noexcept {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace onedrive::detail
