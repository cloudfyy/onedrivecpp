#include "app_state_view_model.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"

#include <utility>

namespace onedrive::gui {

AppStateViewModel load_app_state_view_model(
    const std::filesystem::path& config_file
) {
    const auto config = config::Config::load(config_file);
    const auto token_directory =
        account::AccountState::find_active_token_directory(
            config.state_directory
        );

    auto account_status = std::string{"No active account"};
    if (token_directory) {
        const auth::FileTokenStore token_store{*token_directory};
        account_status = token_store.load_refresh_token() ?
            "Account credentials available" :
            "Active account has no saved credentials";
    }

    return {
        .config_file = config_file,
        .sync_directory = config.sync_data_directory,
        .state_directory = config.state_directory,
        .account_status = std::move(account_status),
    };
}

}  // namespace onedrive::gui
