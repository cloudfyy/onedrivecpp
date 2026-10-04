#pragma once

#include <string>
#include <string_view>

namespace onedrive {

[[nodiscard]] std::string sha256_hex(std::string_view value);

}  // namespace onedrive
