#include "onedrive/account/account_state.hpp"

#include "util/atomic_file.hpp"
#include "util/unique_file_descriptor.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/util/sha256.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace onedrive::account {
namespace {

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

std::string stable_suffix(std::string_view value) {
    return util::sha256_hex(value).substr(0, 8);
}

struct FriendlyComponentInput {
    std::string_view display_name;
    std::string_view stable_id;
    std::string_view fallback;
};

void validate_identity(const DriveIdentity& identity) {
    if (identity.user_id.empty() || identity.user_display_name.empty() ||
        identity.configured_drive_id.empty() || identity.drive_id.empty() ||
        identity.drive_name.empty()) {
        throw std::invalid_argument(
            "account identity requires user and drive IDs and names"
        );
    }
}

std::string friendly_component(FriendlyComponentInput input) {
    const auto [display_name, stable_id, fallback] = input;
    std::string result;
    result.reserve(display_name.size() + 10);
    bool separator = false;
    for (std::size_t index = 0; index < display_name.size();) {
        const auto character =
            static_cast<unsigned char>(display_name[index]);
        if (character >= 0x80U) {
            const std::size_t length =
                (character & 0xE0U) == 0xC0U ? 2 :
                (character & 0xF0U) == 0xE0U ? 3 :
                (character & 0xF8U) == 0xF0U ? 4 : 0;
            bool valid =
                length != 0 && index + length <= display_name.size();
            for (std::size_t offset = 1; valid && offset < length; ++offset) {
                valid =
                    (static_cast<unsigned char>(display_name[index + offset]) &
                     0xC0U) == 0x80U;
            }
            const std::size_t separator_size =
                separator && !result.empty() ? 1 : 0;
            if (!valid) {
                separator = true;
                ++index;
                continue;
            }
            if (result.size() + separator_size + length > 48) {
                break;
            }
            if (separator_size != 0) {
                result.push_back('-');
            }
            result.append(display_name.substr(index, length));
            index += length;
            separator = false;
            continue;
        }
        if (std::isalnum(character) != 0 ||
            character == '_' || character == '-') {
            if (result.size() + (separator && !result.empty() ? 2 : 1) > 48) {
                break;
            }
            if (separator && !result.empty()) {
                result.push_back('-');
            }
            result.push_back(static_cast<char>(character));
            separator = false;
        } else {
            separator = true;
        }
        ++index;
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    if (result.empty()) {
        result = fallback;
    }
    return result + "--" + stable_suffix(stable_id);
}

std::filesystem::path stable_directory(
    const std::filesystem::path& parent,
    std::string_view display_name,
    std::string_view stable_id,
    std::string_view fallback
) {
    const std::string suffix = "--" + stable_suffix(stable_id);
    std::error_code error;
    if (std::filesystem::is_directory(parent, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(parent)) {
            const auto name = entry.path().filename().string();
            if (name.ends_with(suffix) && entry.is_directory() &&
                !entry.is_symlink()) {
                return entry.path();
            }
        }
    }
    return parent /
           friendly_component({
               .display_name = display_name,
               .stable_id = stable_id,
               .fallback = fallback,
           });
}

struct IdentityDirectories {
    std::filesystem::path account;
    std::filesystem::path drive;
};

IdentityDirectories identity_directories(
    const std::filesystem::path& root,
    const DriveIdentity& identity
) {
    validate_identity(identity);
    const auto account = stable_directory(
        root / "accounts",
        identity.user_display_name,
        identity.user_id,
        "account"
    );
    return {
        .account = account,
        .drive = stable_directory(
            account / "drives",
            identity.drive_name,
            identity.drive_id,
            "drive"
        ),
    };
}

void ensure_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    auto status = std::filesystem::symlink_status(path, error);
    bool created = false;
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        created = std::filesystem::create_directory(path);
        status = std::filesystem::symlink_status(path);
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect account state directory '" + path.string() +
            "': " + error.message()
        );
    }
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
        throw std::runtime_error(
            "account state path is not a directory: " + path.string()
        );
    }
    if (::chmod(path.c_str(), private_directory_mode) == -1) {
        throw std::runtime_error(
            "cannot secure account state directory '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    if (created) {
        spdlog::debug("Created account state directory '{}'", path.string());
    }
}

void write_private_file(
    const std::filesystem::path& path,
    std::string_view contents
) {
    onedrive::util::write_file_atomically(
        path,
        contents,
        private_file_mode,
        "account state file"
    );
}

std::string avatar_filename(std::string_view content_type) {
    if (content_type.starts_with("image/png")) {
        return "avatar.png";
    }
    if (content_type.starts_with("image/webp")) {
        return "avatar.webp";
    }
    return "avatar.jpg";
}

AccountPaths paths_for(
    const std::filesystem::path& state_directory,
    const DriveIdentity& identity
) {
    const auto directories = identity_directories(state_directory, identity);
    const auto& account_directory = directories.account;
    const auto& drive_directory = directories.drive;
    std::optional<std::filesystem::path> avatar_path;
    if (identity.photo && !identity.photo->bytes.empty()) {
        avatar_path =
            account_directory /
            avatar_filename(identity.photo->content_type);
    } else {
        for (const auto* name :
             {"avatar.jpg", "avatar.png", "avatar.webp"}) {
            const auto candidate = account_directory / name;
            if (std::filesystem::is_regular_file(candidate)) {
                avatar_path = candidate;
                break;
            }
        }
    }
    return {
        .account_directory = account_directory,
        .drive_directory = drive_directory,
        .token_directory = account_directory,
        .avatar_path = std::move(avatar_path),
    };
}

void write_metadata(
    const AccountPaths& paths,
    const DriveIdentity& identity
) {
    nlohmann::json account{
        {"user_id", identity.user_id},
        {"display_name", identity.user_display_name},
    };
    if (paths.avatar_path) {
        account["avatar_file"] = paths.avatar_path->filename().string();
        account["avatar_content_type"] =
            identity.photo ?
                identity.photo->content_type :
                paths.avatar_path->extension() == ".png" ?
                    "image/png" :
                paths.avatar_path->extension() == ".webp" ?
                    "image/webp" :
                    "image/jpeg";
    }
    write_private_file(
        paths.account_directory / "account.json",
        account.dump(2) + "\n"
    );
    write_private_file(
        paths.drive_directory / "drive.json",
        nlohmann::json{
            {"configured_drive_id", identity.configured_drive_id},
            {"drive_id", identity.drive_id},
            {"name", identity.drive_name},
        }.dump(2) + "\n"
    );
    if (paths.avatar_path && identity.photo) {
        const auto& bytes = identity.photo->bytes;
        write_private_file(
            *paths.avatar_path,
            std::string_view{
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()
            }
        );
    }
}

}  // namespace

AccountPaths AccountState::prepare(
    const std::filesystem::path& state_directory,
    const DriveIdentity& identity
) {
    auto paths = paths_for(state_directory, identity);
    ensure_private_directory(state_directory / "accounts");
    ensure_private_directory(paths.account_directory);
    ensure_private_directory(paths.account_directory / "drives");
    ensure_private_directory(paths.drive_directory);
    write_metadata(paths, identity);
    return paths;
}

AccountPaths AccountState::activate(
    const std::filesystem::path& state_directory,
    const DriveIdentity& identity,
    const std::string& refresh_token
) {
    auto paths = prepare(state_directory, identity);
    auth::FileTokenStore{paths.token_directory}.save_refresh_token(refresh_token);
    write_private_file(
        state_directory / "active_account",
        paths.account_directory.filename().string() + "\n"
    );
    spdlog::info(
        "Activated Microsoft account '{}' for drive '{}'",
        identity.user_display_name,
        identity.drive_name
    );
    return paths;
}

std::filesystem::path AccountState::active_token_directory(
    const std::filesystem::path& state_directory
) {
    const auto directory = find_active_token_directory(state_directory);
    if (!directory) {
        throw std::runtime_error(
            "active Microsoft account is missing; run 'onedrive-cpp auth'"
        );
    }
    return *directory;
}

std::optional<std::filesystem::path>
AccountState::find_active_token_directory(
    const std::filesystem::path& state_directory
) {
    const auto marker = state_directory / "active_account";
    onedrive::util::UniqueFD descriptor{
        ::open(
            marker.c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
        )
    };
    if (!descriptor && errno == ENOENT) {
        return std::nullopt;
    }
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open active Microsoft account marker: " +
            std::string{std::strerror(errno)}
        );
    }
    struct stat marker_status {};
    if (::fstat(descriptor.get(), &marker_status) == -1 ||
        !S_ISREG(marker_status.st_mode) ||
        marker_status.st_uid != ::geteuid()) {
        throw std::runtime_error(
            "active Microsoft account marker must be a regular file owned by "
            "the current user"
        );
    }
    std::array<char, 256> buffer{};
    const auto size =
        ::read(descriptor.get(), buffer.data(), buffer.size());
    const int read_error = errno;
    if (const auto close_error = descriptor.close(); close_error) {
        throw std::runtime_error(
            "cannot close active Microsoft account marker: " +
            close_error.message()
        );
    }
    if (size <= 0 || size == static_cast<ssize_t>(buffer.size())) {
        throw std::runtime_error(
            size == -1 ?
                "cannot read active Microsoft account marker: " +
                    std::string{std::strerror(read_error)} :
                "active Microsoft account marker is empty or too large"
        );
    }
    std::string name{buffer.data(), static_cast<std::size_t>(size)};
    while (!name.empty() &&
           (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw std::runtime_error("active Microsoft account marker is invalid");
    }
    const auto accounts = state_directory / "accounts";
    const auto accounts_status = std::filesystem::symlink_status(accounts);
    if (std::filesystem::is_symlink(accounts_status) ||
        !std::filesystem::is_directory(accounts_status)) {
        throw std::runtime_error(
            "Microsoft accounts state directory is invalid: " +
            accounts.string()
        );
    }
    const auto directory = accounts / name;
    const auto status = std::filesystem::symlink_status(directory);
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
        throw std::runtime_error(
            "active Microsoft account directory is invalid: " +
            directory.string()
        );
    }
    return std::optional{directory};
}

std::filesystem::path AccountState::active_token_path(
    const std::filesystem::path& state_directory
) {
    return active_token_directory(state_directory) / "refresh_token";
}

std::filesystem::path AccountState::drive_data_directory(
    const std::filesystem::path& data_directory,
    const DriveIdentity& identity
) {
    return identity_directories(data_directory, identity).drive;
}

}  // namespace onedrive::account
