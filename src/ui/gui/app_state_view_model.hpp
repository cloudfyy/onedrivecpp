#pragma once

#include <filesystem>
#include <string>

namespace onedrive::gui {

struct AppStateViewModel {
    std::filesystem::path config_file;
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string account_status;
};

[[nodiscard]] AppStateViewModel load_app_state_view_model(
    const std::filesystem::path& config_file
);

}  // namespace onedrive::gui
