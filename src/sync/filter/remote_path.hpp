#pragma once

#include <string_view>

namespace onedrive::sync::detail {

[[nodiscard]] constexpr bool remote_path_is_descendant(
    std::string_view path,
    std::string_view ancestor
) noexcept {
    return path.size() > ancestor.size() &&
           path.starts_with(ancestor) &&
           path[ancestor.size()] == '/';
}

}  // namespace onedrive::sync::detail
