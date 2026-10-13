#pragma once

#include <array>
#include <span>
#include <string_view>

namespace onedrive::graph::client_detail {

struct UnsupportedFormatRule {
    std::string_view id;
    std::string_view display_name;
    std::span<const std::string_view> mime_types;
    std::span<const std::string_view> extensions;
    std::span<const std::string_view> package_types;
};

inline constexpr std::array<std::string_view, 2> onenote_mime_types{
    "application/msonenote", "application/octet-stream"
};
inline constexpr std::array<std::string_view, 2> onenote_extensions{
    ".one", ".onetoc2"
};
inline constexpr std::array<std::string_view, 1> onenote_package_types{
    "oneNote"
};

inline constexpr std::array unsupported_format_rules{
    UnsupportedFormatRule{
        .id = "onenote",
        .display_name = "OneNote",
        .mime_types = onenote_mime_types,
        .extensions = onenote_extensions,
        .package_types = onenote_package_types,
    },
};

} // namespace onedrive::graph::client_detail
