#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace onedrive::account {

struct ProfilePhoto {
    std::string content_type;
    std::vector<std::uint8_t> bytes;
};

struct DriveIdentity {
    std::string user_id;
    std::string user_display_name;
    std::string configured_drive_id;
    std::string drive_id;
    std::string drive_name;
    std::optional<ProfilePhoto> photo;
};

struct AccountPaths {
    std::filesystem::path account_directory;
    std::filesystem::path drive_directory;
    std::filesystem::path token_directory;
    std::optional<std::filesystem::path> avatar_path;
};

class AccountState {
public:
    [[nodiscard]] static AccountPaths activate(
        const std::filesystem::path& state_directory,
        const DriveIdentity& identity,
        const std::string& refresh_token
    );
    [[nodiscard]] static AccountPaths prepare(
        const std::filesystem::path& state_directory,
        const DriveIdentity& identity
    );
    [[nodiscard]] static std::filesystem::path active_token_directory(
        const std::filesystem::path& state_directory
    );
    [[nodiscard]] static std::optional<std::filesystem::path>
    find_active_token_directory(
        const std::filesystem::path& state_directory
    );
    [[nodiscard]] static std::filesystem::path active_token_path(
        const std::filesystem::path& state_directory
    );
    [[nodiscard]] static std::filesystem::path drive_data_directory(
        const std::filesystem::path& data_directory,
        const DriveIdentity& identity
    );
};

}  // namespace onedrive::account
