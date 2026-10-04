#include "onedrive/path_security.hpp"

#include <stdexcept>
#include <string>

namespace onedrive::detail {

std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
) {
    return std::filesystem::absolute(path).lexically_normal();
}

void reject_symlink_components(
    const std::filesystem::path& path,
    std::string_view description
) {
    std::filesystem::path current;
    for (const auto& component : normalized_absolute(path)) {
        current /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            return;
        }
        if (error) {
            throw std::runtime_error(
                "cannot inspect " + std::string{description} + " path '" +
                current.string() + "': " + error.message()
            );
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                std::string{description} +
                " path contains a symbolic link: " + current.string()
            );
        }
    }
}

}  // namespace onedrive::detail
