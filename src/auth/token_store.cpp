#include "onedrive/auth/token_store.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace onedrive::auth {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

TokenStore::TokenStore(std::filesystem::path state_directory)
    : path_{std::move(state_directory) / "refresh_token"} {}

std::optional<std::string> TokenStore::load_refresh_token() const {
    if (!std::filesystem::exists(path_)) {
        return std::nullopt;
    }
    const auto file_status = std::filesystem::symlink_status(path_);
    if (std::filesystem::is_symlink(file_status) ||
        !std::filesystem::is_regular_file(file_status)) {
        throw std::runtime_error(
            "refresh token path is not a regular file: " + path_.string()
        );
    }

    std::ifstream input{path_};
    if (!input) {
        throw std::runtime_error("cannot open refresh token file: " + path_.string());
    }
    std::string token{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    token = trim(std::move(token));
    return token.empty() ? std::nullopt : std::optional<std::string>{std::move(token)};
}

void TokenStore::save_refresh_token(const std::string& refresh_token) const {
    if (refresh_token.empty()) {
        throw std::runtime_error("refusing to persist an empty refresh token");
    }

    std::filesystem::create_directories(path_.parent_path());
    auto temporary_path = path_;
    temporary_path += ".tmp." + std::to_string(::getpid());

    const int descriptor = ::open(
        temporary_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot create refresh token file '" + temporary_path.string() + "': " +
            std::strerror(errno)
        );
    }
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        std::filesystem::remove(temporary_path);
        throw std::runtime_error(
            "cannot secure refresh token file '" + temporary_path.string() + "': " +
            message
        );
    }

    std::size_t written = 0;
    while (written < refresh_token.size()) {
        const auto result = ::write(
            descriptor,
            refresh_token.data() + written,
            refresh_token.size() - written
        );
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            const std::string message = std::strerror(errno);
            ::close(descriptor);
            std::filesystem::remove(temporary_path);
            throw std::runtime_error(
                "cannot write refresh token file '" + temporary_path.string() + "': " +
                message
            );
        }
        written += static_cast<std::size_t>(result);
    }

    if (::fsync(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        std::filesystem::remove(temporary_path);
        throw std::runtime_error(
            "cannot flush refresh token file '" + temporary_path.string() + "': " +
            message
        );
    }
    if (::close(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        std::filesystem::remove(temporary_path);
        throw std::runtime_error(
            "cannot close refresh token file '" + temporary_path.string() + "': " +
            message
        );
    }

    std::error_code error;
    std::filesystem::rename(temporary_path, path_, error);
    if (error) {
        std::filesystem::remove(temporary_path);
        throw std::runtime_error(
            "cannot replace refresh token file '" + path_.string() + "': " +
            error.message()
        );
    }
}

bool TokenStore::remove_refresh_token() const {
    std::error_code error;
    const bool removed = std::filesystem::remove(path_, error);
    if (error) {
        throw std::runtime_error(
            "cannot remove refresh token file '" + path_.string() + "': " +
            error.message()
        );
    }
    return removed;
}

const std::filesystem::path& TokenStore::path() const noexcept {
    return path_;
}

}  // namespace onedrive::auth
