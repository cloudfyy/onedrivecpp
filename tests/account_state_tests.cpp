#include "onedrive/account/account_state.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-account-state-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    const onedrive::account::DriveIdentity identity{
        .user_id = "stable-user-id",
        .user_display_name = "Alice / Example",
        .drive_id = "stable-drive-id",
        .drive_name = "Team / Drive",
        .photo = onedrive::account::ProfilePhoto{
            .content_type = "image/png",
            .bytes = {'p', 'h', 'o', 't', 'o'},
        },
    };
    const auto paths = onedrive::account::AccountState::activate(
        temporary.path(),
        identity,
        "refresh-token"
    );
    if (!paths.account_directory.filename().string().starts_with(
            "Alice-Example--"
        ) ||
        !paths.drive_directory.filename().string().starts_with("Team-Drive--") ||
        !paths.avatar_path ||
        paths.avatar_path->filename() != "avatar.png" ||
        !std::filesystem::exists(paths.account_directory / "account.json") ||
        !std::filesystem::exists(paths.drive_directory / "drive.json") ||
        !std::filesystem::exists(paths.token_directory / "refresh_token") ||
        (std::filesystem::status(paths.account_directory).permissions() &
         std::filesystem::perms::all) !=
            std::filesystem::perms::owner_all ||
        (std::filesystem::status(*paths.avatar_path).permissions() &
         std::filesystem::perms::all) !=
            (std::filesystem::perms::owner_read |
             std::filesystem::perms::owner_write) ||
        onedrive::account::AccountState::active_token_directory(
            temporary.path()
        ) != paths.token_directory) {
        return fail("friendly account state layout was not created");
    }
    std::ifstream avatar{*paths.avatar_path, std::ios::binary};
    const std::string avatar_bytes{
        std::istreambuf_iterator<char>{avatar},
        std::istreambuf_iterator<char>{}
    };
    if (avatar_bytes != "photo") {
        return fail("profile photo was not persisted");
    }

    auto renamed = identity;
    renamed.user_display_name = "Alice Renamed";
    renamed.drive_name = "Renamed Drive";
    renamed.photo.reset();
    const auto existing = onedrive::account::AccountState::prepare(
        temporary.path(),
        renamed
    );
    if (existing.account_directory != paths.account_directory ||
        existing.drive_directory != paths.drive_directory ||
        existing.avatar_path != paths.avatar_path) {
        return fail("stable IDs did not preserve friendly state directories");
    }
    auto international = identity;
    international.user_id = "international-user-id";
    international.user_display_name = "测试 用户";
    international.drive_id = "international-drive-id";
    international.drive_name = "资料";
    international.photo.reset();
    const auto international_paths =
        onedrive::account::AccountState::prepare(
            temporary.path(),
            international
        );
    if (!international_paths.account_directory.filename().string().starts_with(
            "测试-用户--"
        ) ||
        !international_paths.drive_directory.filename().string().starts_with(
            "资料--"
        )) {
        return fail("UTF-8 account names were not preserved safely");
    }
    return EXIT_SUCCESS;
}
