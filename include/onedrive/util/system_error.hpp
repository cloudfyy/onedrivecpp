#pragma once

#include <cerrno>
#include <string>
#include <system_error>

namespace onedrive::util {

[[nodiscard]] inline std::string system_error_message(int error) {
    return std::error_code{error, std::generic_category()}.message();
}

[[noreturn]] inline void throw_system_error(
    int error,
    const std::string& message
) {
    throw std::system_error{
        error, std::generic_category(), message
    };
}

[[noreturn]] inline void throw_errno_error(const std::string& message) {
    throw_system_error(errno, message);
}

}  // namespace onedrive::util
