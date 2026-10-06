#include "http/proxy.hpp"

#include "onedrive/util/path_security.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::http::detail {

using FileDescriptor = onedrive::util::UniqueFD;

std::string read_proxy_password(const std::filesystem::path& path) {
    constexpr std::size_t maximum_password_size =
        std::size_t{64} * 1024U;
    const FileDescriptor descriptor{
        onedrive::util::open_path_no_symlinks(path, O_RDONLY)
    };
    struct stat status {};
    if (::fstat(descriptor.get(), &status) == -1) {
        throw std::runtime_error(
            "cannot inspect proxy password file '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error(
            "proxy password path is not a regular file: " + path.string()
        );
    }
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error(
            "proxy password file must not grant group or other permissions: " +
            path.string()
        );
    }
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) >
            maximum_password_size) {
        throw std::runtime_error(
            "proxy password file exceeds the 64 KiB size limit: " +
            path.string()
        );
    }

    std::string password;
    password.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count =
            ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1) {
            throw std::runtime_error(
                "cannot read proxy password file '" + path.string() +
                "': " + std::strerror(errno)
            );
        }
        if (count == 0) {
            break;
        }
        if (password.size() + static_cast<std::size_t>(count) >
            maximum_password_size) {
            throw std::runtime_error(
                "proxy password file exceeds the 64 KiB size limit: " +
                path.string()
            );
        }
        password.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (password.ends_with('\n')) {
        password.pop_back();
        if (password.ends_with('\r')) {
            password.pop_back();
        }
    }
    if (password.empty()) {
        throw std::runtime_error(
            "proxy password file must not be empty: " + path.string()
        );
    }
    if (password.contains('\0')) {
        throw std::runtime_error(
            "proxy password file must not contain NUL bytes: " +
            path.string()
        );
    }
    return password;
}

std::string join_proxy_bypass_list(
    const std::vector<std::string>& entries
) {
    std::string result;
    for (const auto& entry : entries) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result += entry;
    }
    return result;
}

}  // namespace onedrive::http::detail
