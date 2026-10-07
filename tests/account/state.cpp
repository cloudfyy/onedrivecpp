#include "onedrive/account/account_state.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

template <typename Operation>
bool throws_with(Operation&& operation, std::string_view expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        return std::string_view{error.what()}.contains(expected);
    }
    return false;
}

bool rejected_active_marker(
    const std::filesystem::path& root,
    std::string_view marker,
    std::string_view expected
) {
    std::filesystem::create_directories(root);
    onedrive::test::write_file(root / "active_account", marker);
    return throws_with(
        [&] {
            static_cast<void>(
                onedrive::account::AccountState::find_active_token_directory(
                    root
                )
            );
        },
        expected
    );
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    const onedrive::account::DriveIdentity identity{
        .user_id = "stable-user-id",
        .user_display_name = "Alice / Example",
        .configured_drive_id = "me",
        .drive_id = "stable-drive-id",
        .drive_name = "Team / Drive",
        .photo = onedrive::account::ProfilePhoto{
            .content_type = "image/png",
            .bytes = {'p', 'h', 'o', 't', 'o'},
        },
    };
    const auto paths = onedrive::account::AccountState::activate(
        temporary.path(), identity, "refresh-token"
    );
    if (!paths.account_directory.filename().string().starts_with(
            "Alice-Example--"
        ) ||
        !paths.drive_directory.filename().string().starts_with(
            "Team-Drive--"
        ) ||
        !paths.avatar_path || paths.avatar_path->filename() != "avatar.png" ||
        !std::filesystem::exists(paths.account_directory / "account.json") ||
        !std::filesystem::exists(paths.drive_directory / "drive.json") ||
        !std::filesystem::exists(paths.token_directory / "refresh_token") ||
        (std::filesystem::status(paths.account_directory).permissions() &
         std::filesystem::perms::all) != std::filesystem::perms::owner_all ||
        (std::filesystem::status(*paths.avatar_path).permissions() &
         std::filesystem::perms::all) !=
            (std::filesystem::perms::owner_read |
             std::filesystem::perms::owner_write) ||
        onedrive::account::AccountState::active_token_directory(temporary.path()
        ) != paths.token_directory) {
        return fail("friendly account state layout was not created");
    }
    const auto avatar_bytes = onedrive::test::read_file(*paths.avatar_path);
    if (avatar_bytes != "photo") {
        return fail("profile photo was not persisted");
    }
    const auto drive_metadata_contents =
        onedrive::test::read_file(paths.drive_directory / "drive.json");
    if (!drive_metadata_contents.contains(R"("configured_drive_id": "me")") ||
        !drive_metadata_contents.contains(R"("drive_id": "stable-drive-id")")) {
        return fail("configured Drive ID mapping was not saved as metadata");
    }

    auto renamed = identity;
    renamed.user_display_name = "Alice Renamed";
    renamed.drive_name = "Renamed Drive";
    renamed.photo.reset();
    const auto existing =
        onedrive::account::AccountState::prepare(temporary.path(), renamed);
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
    const auto international_paths = onedrive::account::AccountState::prepare(
        temporary.path(), international
    );
    if (!international_paths.account_directory.filename().string().starts_with(
            "测试-用户--"
        ) ||
        !international_paths.drive_directory.filename().string().starts_with(
            "资料--"
        )) {
        return fail("UTF-8 account names were not preserved safely");
    }

    const auto data_root = temporary.path() / "data";
    const auto data_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, identity
        );
    std::filesystem::create_directories(data_directory);
    const auto renamed_data_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, renamed
        );
    auto second_drive = identity;
    second_drive.drive_id = "second-drive-id";
    const auto second_drive_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, second_drive
        );
    auto same_name_user = identity;
    same_name_user.user_id = "second-user-id";
    same_name_user.drive_id = "second-user-drive-id";
    const auto same_name_user_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, same_name_user
        );
    const auto second_user_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, international
        );
    if (data_directory != renamed_data_directory ||
        data_directory.parent_path().filename() != "drives" ||
        data_directory.parent_path().parent_path().parent_path() !=
            data_root / "accounts" ||
        second_drive_directory == data_directory ||
        second_drive_directory.parent_path() != data_directory.parent_path() ||
        same_name_user_directory.parent_path().parent_path() ==
            data_directory.parent_path().parent_path() ||
        second_user_directory.parent_path().parent_path() ==
            data_directory.parent_path().parent_path()) {
        return fail("account and Drive data directories were not isolated");
    }
    try {
        static_cast<void>(
            onedrive::account::AccountState::drive_data_directory(data_root, {})
        );
        return fail("incomplete account identity produced a data directory");
    } catch (const std::invalid_argument&) {
    }

    auto webp_identity = identity;
    webp_identity.user_id = "webp-user";
    webp_identity.drive_id = "webp-drive";
    webp_identity.photo = onedrive::account::ProfilePhoto{
        .content_type = "image/webp",
        .bytes = {'w'},
    };
    const auto webp_paths = onedrive::account::AccountState::prepare(
        temporary.path(), webp_identity
    );
    auto jpeg_identity = identity;
    jpeg_identity.user_id = "jpeg-user";
    jpeg_identity.drive_id = "jpeg-drive";
    jpeg_identity.photo = onedrive::account::ProfilePhoto{
        .content_type = "image/jpeg; charset=binary",
        .bytes = {'j'},
    };
    const auto jpeg_paths = onedrive::account::AccountState::prepare(
        temporary.path(), jpeg_identity
    );
    auto fallback_identity = identity;
    fallback_identity.user_id = "fallback-user";
    fallback_identity.user_display_name = "///";
    fallback_identity.drive_id = "fallback-drive";
    fallback_identity.drive_name = std::string{"\xF0\x28\x8C\x28", 4};
    fallback_identity.photo.reset();
    const auto fallback_drive_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, fallback_identity
        );
    auto long_identity = identity;
    long_identity.user_id = "long-user";
    long_identity.user_display_name = std::string(60, 'a');
    long_identity.drive_id = "long-drive";
    long_identity.drive_name.clear();
    for (int index = 0; index < 30; ++index) {
        long_identity.drive_name += "测";
    }
    const auto long_drive_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, long_identity
        );
    auto trailing_identity = identity;
    trailing_identity.user_id = "trailing-user";
    trailing_identity.user_display_name = "trailing ///";
    trailing_identity.drive_id = "trailing-drive";
    trailing_identity.drive_name = "trailing ///";
    const auto trailing_drive_directory =
        onedrive::account::AccountState::drive_data_directory(
            data_root, trailing_identity
        );
    if (!webp_paths.avatar_path ||
        webp_paths.avatar_path->filename() != "avatar.webp" ||
        !jpeg_paths.avatar_path ||
        jpeg_paths.avatar_path->filename() != "avatar.jpg" ||
        !fallback_drive_directory.parent_path()
             .parent_path()
             .filename()
             .string()
             .starts_with("account--") ||
        !fallback_drive_directory.filename().string().starts_with("drive--") ||
        long_drive_directory.parent_path()
                .parent_path()
                .filename()
                .string()
                .size() > 58 ||
        long_drive_directory.filename().string().size() > 58 ||
        trailing_drive_directory.parent_path()
            .parent_path()
            .filename()
            .string()
            .contains("---") ||
        trailing_drive_directory.filename().string().contains("---")) {
        return fail("account state friendly filenames were incorrect");
    }

    auto existing_webp_identity = identity;
    existing_webp_identity.user_id = "existing-webp-user";
    existing_webp_identity.drive_id = "existing-webp-drive";
    existing_webp_identity.photo.reset();
    const auto existing_webp_paths = onedrive::account::AccountState::locate(
        temporary.path(), existing_webp_identity
    );
    std::filesystem::create_directories(existing_webp_paths.account_directory);
    onedrive::test::write_file(
        existing_webp_paths.account_directory / "avatar.webp", "webp"
    );
    const auto prepared_existing_webp =
        onedrive::account::AccountState::prepare(
            temporary.path(), existing_webp_identity
        );
    if (!prepared_existing_webp.avatar_path ||
        prepared_existing_webp.avatar_path->filename() != "avatar.webp") {
        return fail("existing WebP account avatar was not preserved");
    }

    const auto invalid_accounts_root =
        temporary.path() / "invalid-accounts-root";
    std::filesystem::create_directory(invalid_accounts_root);
    onedrive::test::write_file(
        invalid_accounts_root / "accounts", "not a directory"
    );
    if (!throws_with(
            [&] {
                static_cast<void>(onedrive::account::AccountState::prepare(
                    invalid_accounts_root, identity
                ));
            },
            "account state path is not a directory"
        )) {
        return fail("account state file was accepted as a directory");
    }

    const auto missing_state = temporary.path() / "missing-active";
    if (onedrive::account::AccountState::find_active_token_directory(
            missing_state
        ) ||
        !throws_with(
            [&] {
                static_cast<void>(
                    onedrive::account::AccountState::active_token_directory(
                        missing_state
                    )
                );
            },
            "active Microsoft account is missing"
        )) {
        return fail("missing active account marker was accepted");
    }

    const auto marker_directory = temporary.path() / "marker-directory";
    std::filesystem::create_directories(marker_directory / "active_account");
    if (!throws_with(
            [&] {
                static_cast<void>(
                    onedrive::account::AccountState::
                        find_active_token_directory(marker_directory)
                );
            },
            "must be a regular file"
        )) {
        return fail("directory active account marker was accepted");
    }

    const auto marker_symlink = temporary.path() / "marker-symlink";
    std::filesystem::create_directories(marker_symlink);
    std::filesystem::create_symlink(
        paths.account_directory.filename(), marker_symlink / "active_account"
    );
    if (!throws_with(
            [&] {
                static_cast<void>(
                    onedrive::account::AccountState::
                        find_active_token_directory(marker_symlink)
                );
            },
            "cannot open active Microsoft account marker"
        )) {
        return fail("symbolic-link active account marker was accepted");
    }

    if (!rejected_active_marker(
            temporary.path() / "empty-marker", "", "empty or too large"
        ) ||
        !rejected_active_marker(
            temporary.path() / "large-marker",
            std::string(256, 'a'),
            "empty or too large"
        )) {
        return fail("empty or oversized active account marker was accepted");
    }
    for (const std::string_view invalid : {
             ".\n",
             "..\n",
             "account/name\n",
             "account\\name\n",
         }) {
        if (!rejected_active_marker(
                temporary.path() /
                    ("invalid-marker-" + std::to_string(invalid.size())),
                invalid,
                "marker is invalid"
            )) {
            return fail("unsafe active account marker content was accepted");
        }
    }

    const auto missing_accounts = temporary.path() / "missing-accounts";
    if (!rejected_active_marker(
            missing_accounts, "account\n", "accounts state directory is invalid"
        )) {
        return fail("missing accounts directory was accepted");
    }
    const auto linked_accounts = temporary.path() / "linked-accounts";
    std::filesystem::create_directories(linked_accounts);
    std::filesystem::create_directory_symlink(
        paths.account_directory.parent_path(), linked_accounts / "accounts"
    );
    onedrive::test::write_file(linked_accounts / "active_account", "account\n");
    if (!throws_with(
            [&] {
                static_cast<void>(
                    onedrive::account::AccountState::
                        find_active_token_directory(linked_accounts)
                );
            },
            "accounts state directory is invalid"
        )) {
        return fail("symbolic-link accounts directory was accepted");
    }

    const auto missing_account = temporary.path() / "missing-account";
    std::filesystem::create_directories(missing_account / "accounts");
    onedrive::test::write_file(
        missing_account / "active_account", "account\r\n"
    );
    if (!throws_with(
            [&] {
                static_cast<void>(
                    onedrive::account::AccountState::
                        find_active_token_directory(missing_account)
                );
            },
            "active Microsoft account directory is invalid"
        )) {
        return fail("missing active account directory was accepted");
    }
    std::filesystem::create_directory(missing_account / "accounts" / "account");
    if (onedrive::account::AccountState::find_active_token_directory(
            missing_account
        ) != missing_account / "accounts" / "account" ||
        onedrive::account::AccountState::active_token_path(missing_account) !=
            missing_account / "accounts" / "account" / "refresh_token") {
        return fail("valid CRLF active account marker was not resolved");
    }
    return EXIT_SUCCESS;
}
