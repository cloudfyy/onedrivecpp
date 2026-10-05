#pragma once

#include <chrono>
#include <string_view>

namespace onedrive::util {

[[nodiscard]] std::chrono::sys_time<std::chrono::nanoseconds>
parse_remote_modified_time(std::string_view timestamp);

}  // namespace onedrive::util
