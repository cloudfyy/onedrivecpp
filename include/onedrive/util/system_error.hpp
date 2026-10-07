#pragma once

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace onedrive::util {

[[noreturn]] inline void throw_system_error(
    int error,
    std::string message
) {
    throw std::system_error{
        error, std::generic_category(), std::move(message)
    };
}

[[noreturn]] inline void throw_errno_error(std::string message) {
    throw_system_error(errno, std::move(message));
}

}  // namespace onedrive::util
