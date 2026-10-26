#include "onedrive/auth/token_store.hpp"

#include "onedrive/util/path_security.hpp"
#include "util/ascii.hpp"
#include "util/atomic_file.hpp"
#include "util/private_file.hpp"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::auth {

FileTokenStore::FileTokenStore(const std::filesystem::path& state_directory)
    : path_{state_directory / "refresh_token"} {
}

std::optional<std::string> FileTokenStore::load_refresh_token() const {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path_, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) {
        spdlog::debug("No persisted Microsoft refresh token is present");
        return std::nullopt;
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect refresh token file '" + path_.string() +
            "': " + error.message()
        );
    }
    std::string token = onedrive::util::read_private_file(
        path_,
        "refresh token",
        {
            .required_owner = ::geteuid(),
            .exact_permissions = S_IRUSR | S_IWUSR,
            .forbidden_permissions = 0,
            .permission_requirement =
                "must be owned by the current user with 0600 permissions",
            .require_single_link = true,
        }
    );
    token = std::string{onedrive::util::trim_ascii_whitespace(token)};
    spdlog::debug(
        "Loaded persisted Microsoft refresh token: {}",
        token.empty() ? "empty" : "present"
    );
    return token.empty() ? std::nullopt
                         : std::optional<std::string>{std::move(token)};
}

void FileTokenStore::save_refresh_token(
    const std::string& refresh_token
) const {
    if (refresh_token.empty()) {
        throw std::runtime_error("refusing to persist an empty refresh token");
    }

    const auto parent = path_.parent_path();
    onedrive::util::reject_symlink_components(
        parent, "refresh token directory"
    );
    std::filesystem::create_directories(parent);
    onedrive::util::reject_symlink_components(
        parent, "refresh token directory"
    );
    onedrive::util::secure_owned_directory(
        parent, S_IRWXU, "refresh token directory"
    );
    onedrive::util::write_file_atomically(
        path_, refresh_token, S_IRUSR | S_IWUSR, "refresh token file"
    );
    spdlog::debug("Persisted Microsoft refresh token securely");
}

bool FileTokenStore::remove_refresh_token() const {
    std::error_code error;
    const bool removed = std::filesystem::remove(path_, error);
    if (error) {
        throw std::runtime_error(
            "cannot remove refresh token file '" + path_.string() +
            "': " + error.message()
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

} // namespace onedrive::auth
