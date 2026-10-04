#include "onedrive/auth/token_store.hpp"

#include "detail/atomic_file.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::auth {
namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

FileTokenStore::FileTokenStore(const std::filesystem::path& state_directory)
    : path_{state_directory / "refresh_token"} {}

std::optional<std::string> FileTokenStore::load_refresh_token() const {
    if (!std::filesystem::exists(path_)) {
        spdlog::debug("No persisted Microsoft refresh token is present");
        return std::nullopt;
    }
    const auto file_status = std::filesystem::symlink_status(path_);
    if (std::filesystem::is_symlink(file_status) ||
        !std::filesystem::is_regular_file(file_status)) {
        throw std::runtime_error(
            "refresh token path is not a regular file: " + path_.string()
        );
    }
    struct stat status {};
    if (::lstat(path_.c_str(), &status) == -1) {
        throw std::runtime_error(
            "cannot inspect refresh token file '" + path_.string() + "': " +
            std::strerror(errno)
        );
    }
    if (status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != (S_IRUSR | S_IWUSR)) {
        throw std::runtime_error(
            "refresh token file must be owned by the current user with 0600 "
            "permissions: " + path_.string()
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
    token = trim(token);
    spdlog::debug(
        "Loaded persisted Microsoft refresh token: {}",
        token.empty() ? "empty" : "present"
    );
    return token.empty() ? std::nullopt : std::optional<std::string>{std::move(token)};
}

void FileTokenStore::save_refresh_token(const std::string& refresh_token) const {
    if (refresh_token.empty()) {
        throw std::runtime_error("refusing to persist an empty refresh token");
    }

    std::filesystem::create_directories(path_.parent_path());
    onedrive::detail::write_file_atomically(
        path_,
        refresh_token,
        S_IRUSR | S_IWUSR,
        "refresh token file"
    );
    spdlog::debug("Persisted Microsoft refresh token securely");
}

bool FileTokenStore::remove_refresh_token() const {
    std::error_code error;
    const bool removed = std::filesystem::remove(path_, error);
    if (error) {
        throw std::runtime_error(
            "cannot remove refresh token file '" + path_.string() + "': " +
            error.message()
        );
    }
    spdlog::debug(
        "Removed persisted Microsoft refresh token: {}",
        removed ? "present" : "not present"
    );
    return removed;
}

const std::filesystem::path& FileTokenStore::path() const noexcept {
    return path_;
}

}  // namespace onedrive::auth
