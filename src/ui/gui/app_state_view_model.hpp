#pragma once
#include "onedrive/sync/capabilities.hpp"
#include "onedrive/config/console.hpp"

#include <filesystem>
#include <string>

namespace onedrive::gui {

enum class AccountStatus {
    no_account, credentials_available, no_credentials, unavailable, invalid
};

enum class ConfigurationStatus { valid, missing, invalid };

struct AppStateViewModel {
    std::filesystem::path config_file;
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    sync::SyncMode sync_mode{sync::SyncMode::bidirectional};
    std::size_t file_history_limit{config::default_file_history_limit};
    AccountStatus account_status{AccountStatus::no_account};
    ConfigurationStatus configuration_status{ConfigurationStatus::valid};
    std::string configuration_error;
    std::string authentication_error;
    std::string account_error;
    std::string sync_error;
    std::string drive_type;

    [[nodiscard]] bool can_sign_in() const {
        return configuration_status != ConfigurationStatus::invalid &&
               authentication_error.empty() && account_error.empty();
    }

    [[nodiscard]] bool can_synchronize() const {
        return configuration_status == ConfigurationStatus::valid &&
               can_sign_in() && sync_error.empty() &&
               account_status == AccountStatus::credentials_available;
    }
};

[[nodiscard]] AppStateViewModel load_app_state_view_model(
    const std::filesystem::path& config_file
);

}  // namespace onedrive::gui
