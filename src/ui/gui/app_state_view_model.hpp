#pragma once

#include <filesystem>

namespace onedrive::gui {

enum class AccountStatus { no_account, credentials_available, no_credentials };

struct AppStateViewModel {
    std::filesystem::path config_file;
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    AccountStatus account_status{AccountStatus::no_account};
};

[[nodiscard]] AppStateViewModel load_app_state_view_model(
    const std::filesystem::path& config_file
);

}  // namespace onedrive::gui
