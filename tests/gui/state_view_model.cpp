#include "app_state_view_model.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/config/config.hpp"
#include "support/common.hpp"

namespace {

using onedrive::test::TemporaryDirectory;

int test_view_model_without_account() {
    TemporaryDirectory temporary;
    const auto config_file = temporary.path() / "config.toml";
    const auto state_directory = temporary.path() / "state";
    const auto sync_directory = temporary.path() / "OneDrive";
    onedrive::test::write_file(
        config_file,
        "config_version = 2\n"
        "[sync]\n"
        "data_directory = \"" + sync_directory.string() + "\"\n"
        "[state]\n"
        "directory = \"" + state_directory.string() + "\"\n"
    );

    const auto state =
        onedrive::gui::load_app_state_view_model(config_file);
    if (state.config_file != config_file ||
        state.sync_directory != sync_directory ||
        state.state_directory != state_directory ||
        state.account_status != "No active account") {
        return onedrive::test::fail(
            "GUI view model did not report configured paths and missing account"
        );
    }
    return EXIT_SUCCESS;
}

int test_view_model_with_saved_credentials() {
    TemporaryDirectory temporary;
    const auto config_file = temporary.path() / "config.toml";
    const auto state_directory = temporary.path() / "state";
    onedrive::test::write_file(
        config_file,
        "config_version = 2\n"
        "[state]\n"
        "directory = \"" + state_directory.string() + "\"\n"
    );
    std::filesystem::create_directories(state_directory);
    const auto account_paths = onedrive::account::AccountState::activate(
        state_directory,
        {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = "me",
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        },
        "refresh-token"
    );
    if (!std::filesystem::exists(
            account_paths.token_directory / "refresh_token"
        )) {
        return onedrive::test::fail(
            "test account credentials were not persisted"
        );
    }

    const auto state =
        onedrive::gui::load_app_state_view_model(config_file);
    if (state.account_status != "Account credentials available") {
        return onedrive::test::fail(
            "GUI view model did not detect saved account credentials"
        );
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_view_model_without_account() != EXIT_SUCCESS ||
        test_view_model_with_saved_credentials() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
