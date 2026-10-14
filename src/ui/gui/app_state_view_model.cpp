#include "app_state_view_model.hpp"
#include "configuration.hpp"

#include "app/checks.hpp"
#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"

#include <stdexcept>

namespace onedrive::gui {

AppStateViewModel load_app_state_view_model(
    const std::filesystem::path& config_file
) {
    AppStateViewModel state;
    state.config_file = config_file;
    config::Config config;
    bool missing = false;
    try {
        config = load_configuration(config_file, missing);
    } catch (const std::runtime_error& error) {
        state.configuration_status = ConfigurationStatus::invalid;
        state.configuration_error = error.what();
        state.account_status = AccountStatus::unavailable;
        return state;
    }
    state.configuration_status =
        missing ? ConfigurationStatus::missing : ConfigurationStatus::valid;
    state.sync_directory = config.sync_data_directory;
    state.state_directory = config.state_directory;
    state.sync_mode = config.sync_mode;
    try {
        app::detail::validate_authentication_config(config);
    } catch (const std::runtime_error& error) {
        state.authentication_error = error.what();
    }
    try {
        config::validate_sync_state_directories(
            config.sync_data_directory, config.state_directory
        );
    } catch (const std::runtime_error& error) {
        state.sync_error = error.what();
    }
    try {
        app::detail::inspect_state_directory(config.state_directory);
        const auto token_directory =
            account::AccountState::find_active_token_directory(
                config.state_directory
            );
        if (token_directory) {
            const auth::FileTokenStore token_store{*token_directory};
            const auto token = token_store.load_refresh_token();
            if (!token && std::filesystem::exists(
                              std::filesystem::symlink_status(token_store.path())
                          )) {
                throw std::runtime_error("saved refresh token is empty");
            }
            state.account_status = token ? AccountStatus::credentials_available
                                         : AccountStatus::no_credentials;
        }
    } catch (const std::runtime_error& error) {
        state.account_status = AccountStatus::invalid;
        state.account_error = error.what();
    }
    return state;
}

}  // namespace onedrive::gui
