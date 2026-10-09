#include "app_state_view_model.hpp"
#include "configuration.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/config/config.hpp"
#include "util/private_file.hpp"
#include "support/common.hpp"

#include <sys/stat.h>

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
        "data_directory = \"" +
            sync_directory.string() +
            "\"\n"
            "[state]\n"
            "directory = \"" +
            state_directory.string() + "\"\n"
    );

    const auto state = onedrive::gui::load_app_state_view_model(config_file);
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
        "directory = \"" +
            state_directory.string() + "\"\n"
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

    const auto state = onedrive::gui::load_app_state_view_model(config_file);
    if (state.account_status != "Account credentials available") {
        return onedrive::test::fail(
            "GUI view model did not detect saved account credentials"
        );
    }
    return EXIT_SUCCESS;
}

int test_basic_settings_preserve_other_toml() {
    TemporaryDirectory temporary;
    const auto config_file = temporary.path() / "config.toml";
    const auto source =
        "# Keep operator notes\n"
        "config_version = 2\n"
        "\n"
        "[sync]\n"
        "data_directory = \"/old/OneDrive\" # existing comment\n"
        "mode = \"upload_only\"\n"
        "\n"
        "[state]\n"
        "directory = \"/old/state\"\n";
    const auto sync_directory = temporary.path() / "new OneDrive";
    const auto state_directory = temporary.path() / "new state";
    const auto updated = onedrive::config::update_basic_paths(
        config_file, source, sync_directory, state_directory
    );
    const auto loaded =
        onedrive::config::Config::load_from_string(config_file, updated);
    if (!updated.contains("# Keep operator notes") ||
        !updated.contains("# existing comment") ||
        loaded.sync_data_directory != sync_directory ||
        loaded.state_directory != state_directory ||
        loaded.sync_mode != onedrive::sync::SyncMode::upload_only) {
        return onedrive::test::fail(
            "basic settings edit did not preserve TOML comments and other "
            "values"
        );
    }
    return EXIT_SUCCESS;
}

int test_basic_settings_add_missing_tables() {
    TemporaryDirectory temporary;
    const auto config_file = temporary.path() / "config.toml";
    const auto updated = onedrive::config::update_basic_paths(
        config_file,
        "config_version = 2\n",
        temporary.path() / "OneDrive",
        temporary.path() / "state"
    );
    const auto loaded =
        onedrive::config::Config::load_from_string(config_file, updated);
    if (loaded.sync_data_directory != temporary.path() / "OneDrive" ||
        loaded.state_directory != temporary.path() / "state") {
        return onedrive::test::fail(
            "basic settings edit did not add missing TOML tables"
        );
    }
    return EXIT_SUCCESS;
}

int test_basic_settings_add_key_to_existing_table() {
    TemporaryDirectory temporary;
    const auto config_file = temporary.path() / "config.toml";
    const auto updated = onedrive::config::update_basic_paths(
        config_file,
        "config_version = 2\n"
        "[sync]\n"
        "mode = \"download_only\"\n"
        "\n"
        "[state]\n",
        temporary.path() / "OneDrive",
        temporary.path() / "state"
    );
    const auto loaded =
        onedrive::config::Config::load_from_string(config_file, updated);
    if (loaded.sync_mode != onedrive::sync::SyncMode::download_only ||
        loaded.sync_data_directory != temporary.path() / "OneDrive" ||
        loaded.state_directory != temporary.path() / "state") {
        return onedrive::test::fail(
            "basic settings edit did not add paths to existing tables"
        );
    }
    return EXIT_SUCCESS;
}

int test_manual_settings_create_first_config() {
    TemporaryDirectory temporary;
    const auto config_file =
        temporary.path() / ".config/onedrive-cpp/config.toml";
    const auto backup = onedrive::gui::save_basic_settings(
        config_file,
        temporary.path() / "OneDrive",
        temporary.path() / ".local/state/onedrive-cpp"
    );
    if (backup) {
        return onedrive::test::fail(
            "first configuration save unexpectedly created a backup"
        );
    }
    const auto loaded = onedrive::config::Config::load(config_file);
    if (loaded.sync_data_directory != temporary.path() / "OneDrive" ||
        loaded.state_directory !=
            temporary.path() / ".local/state/onedrive-cpp") {
        return onedrive::test::fail(
            "manual setup did not create a valid initial configuration"
        );
    }
    return EXIT_SUCCESS;
}

int test_import_and_atomic_save() {
    TemporaryDirectory temporary;
    const auto source_file = temporary.path() / "import.toml";
    const auto destination =
        temporary.path() / ".config/onedrive-cpp/config.toml";
    const auto source_contents = "# imported settings\n"
                                 "config_version = 2\n"
                                 "[sync]\n"
                                 "data_directory = \"/imported/OneDrive\"\n"
                                 "[state]\n"
                                 "directory = \"/imported/state\"\n";
    onedrive::test::write_file(source_file, source_contents);
    const auto preview =
        onedrive::gui::preview_configuration(source_file, destination);
    if (preview.sync_directory != "/imported/OneDrive" ||
        preview.state_directory != "/imported/state" ||
        preview.sync_mode != "Bidirectional") {
        return onedrive::test::fail(
            "configuration import preview returned incorrect values"
        );
    }

    const auto first_backup =
        onedrive::gui::import_configuration(source_file, destination);
    if (first_backup ||
        onedrive::test::read_file(destination) != source_contents) {
        return onedrive::test::fail(
            "initial configuration import did not preserve the TOML file"
        );
    }
    const auto backup = onedrive::gui::save_basic_settings(
        destination, "/saved/OneDrive", "/saved/state"
    );
    if (!backup || !std::filesystem::exists(*backup) ||
        !onedrive::test::read_file(*backup).contains("# imported settings")) {
        return onedrive::test::fail(
            "saving settings did not create a complete configuration backup"
        );
    }
    struct stat status{};
    if (::stat(destination.c_str(), &status) == -1 ||
        (status.st_mode & 0777) != 0600) {
        return onedrive::test::fail(
            "saved configuration file permissions are not private"
        );
    }
    const auto loaded = onedrive::config::Config::load(destination);
    if (loaded.sync_data_directory != "/saved/OneDrive" ||
        loaded.state_directory != "/saved/state") {
        return onedrive::test::fail(
            "saved basic paths are not active in the configuration"
        );
    }

    const auto before_invalid_save = onedrive::test::read_file(destination);
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::gui::save_basic_settings(
                    destination, "/saved", "/saved/state"
                ));
            },
            "must not contain one another"
        ) ||
        onedrive::test::read_file(destination) != before_invalid_save) {
        return onedrive::test::fail(
            "invalid manual settings changed the active configuration"
        );
    }

    const auto invalid_file = temporary.path() / "invalid.toml";
    onedrive::test::write_file(
        invalid_file,
        "config_version = 2\n"
        "[sync]\n"
        "data_directory = \"/bad\"\n"
        "[state]\n"
        "directory = \"/bad/state\"\n"
    );
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::gui::import_configuration(
                    invalid_file, destination
                ));
            },
            "must not contain one another"
        ) ||
        onedrive::test::read_file(destination) ==
            onedrive::test::read_file(invalid_file)) {
        return onedrive::test::fail(
            "invalid configuration import replaced the active configuration"
        );
    }
    return EXIT_SUCCESS;
}

int test_shared_config_file_reader_limits() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "large.toml";
    const std::string contents(70U * 1024U, 'x');
    onedrive::test::write_file(path, contents);
    try {
        static_cast<void>(onedrive::util::read_private_file(
            path,
            "configuration",
            {
                .maximum_size = 64U * 1024U,
            }
        ));
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.contains("size limit")) {
            return EXIT_SUCCESS;
        }
        return onedrive::test::fail(
            "shared file reader returned an unexpected size error"
        );
    }
    return onedrive::test::fail(
        "shared file reader did not enforce the caller's size limit"
    );
}

} // namespace

int main() {
    if (test_view_model_without_account() != EXIT_SUCCESS ||
        test_view_model_with_saved_credentials() != EXIT_SUCCESS ||
        test_basic_settings_preserve_other_toml() != EXIT_SUCCESS ||
        test_basic_settings_add_missing_tables() != EXIT_SUCCESS ||
        test_basic_settings_add_key_to_existing_table() != EXIT_SUCCESS ||
        test_manual_settings_create_first_config() != EXIT_SUCCESS ||
        test_import_and_atomic_save() != EXIT_SUCCESS ||
        test_shared_config_file_reader_limits() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
