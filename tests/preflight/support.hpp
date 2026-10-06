#include "app/preflight.hpp"

#include "onedrive/config/config.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <stdexcept>
#include <string>

namespace onedrive::test::preflight {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

onedrive::config::Config config_for(const TemporaryDirectory& temporary) {
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "files";
    config.application_id = "test-application";
    config.azure_tenant_id = "common";
    config.auth_endpoint = "https://login.example.test";
    config.auth_scope = "User.Read Files.ReadWrite offline_access";
    config.drive_id = "me";
    return config;
}

std::filesystem::path write_token(
    const std::filesystem::path& state_directory,
    std::filesystem::perms permissions
) {
    const auto account_directory =
        state_directory / "accounts/Test-User--12345678";
    std::filesystem::create_directories(account_directory);
    {
        std::ofstream marker{state_directory / "active_account"};
        marker << account_directory.filename().string() << '\n';
    }
    const auto token_path = account_directory / "refresh_token";
    std::ofstream output{token_path};
    output << "test-refresh-token";
    output.close();
    std::filesystem::permissions(token_path, permissions);
    return token_path;
}

template <typename Action>
bool throws_with(Action action, const std::string& expected) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        return std::string{error.what()}.contains(expected);
    }
    return false;
}

} // namespace onedrive::test::preflight
