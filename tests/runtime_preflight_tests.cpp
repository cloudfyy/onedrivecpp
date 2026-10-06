#include "app/runtime_preflight.hpp"

#include "onedrive/config/config.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <stdexcept>
#include <string>

namespace {

using onedrive::test::TemporaryDirectory;
using onedrive::test::fail;

onedrive::config::Config config_for(const TemporaryDirectory& temporary) {
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_directory = temporary.path() / "files";
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

int test_private_state_and_lock() {
    using onedrive::app::detail::Operation;
    using onedrive::app::detail::RuntimePreflight;

    TemporaryDirectory temporary;
    const auto config = config_for(temporary);
    {
        const RuntimePreflight first{config, Operation::logout};
        const auto state_permissions =
            std::filesystem::status(config.state_directory).permissions();
        const auto lock_permissions =
            std::filesystem::status(
                config.state_directory / "onedrive-cpp.lock"
            ).permissions();
        if ((state_permissions & std::filesystem::perms::all) !=
                std::filesystem::perms::owner_all ||
            (lock_permissions & std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail("preflight did not secure state paths");
        }
        if (!throws_with(
                [&] {
                    const RuntimePreflight second{config, Operation::logout};
                },
                "another onedrive-cpp process"
            )) {
            return fail("concurrent state-directory use was accepted");
        }
    }
    const RuntimePreflight after_release{config, Operation::logout};
    return EXIT_SUCCESS;
}

int test_sync_directory_and_token() {
    using onedrive::app::detail::Operation;
    using onedrive::app::detail::RuntimePreflight;

    TemporaryDirectory temporary;
    auto config = config_for(temporary);
    const auto token_path = write_token(
        config.state_directory,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read
    );
    {
        const RuntimePreflight preflight{config, Operation::synchronize};
        if (!std::filesystem::is_directory(config.sync_directory)) {
            return fail("sync preflight did not create the sync directory");
        }
        const auto sync_permissions =
            std::filesystem::status(config.sync_directory).permissions();
        if ((sync_permissions & std::filesystem::perms::all) !=
            std::filesystem::perms::owner_all) {
            return fail("sync preflight did not secure the sync directory");
        }
        const auto token_permissions =
            std::filesystem::status(token_path).permissions();
        if ((token_permissions & std::filesystem::perms::all) !=
            (std::filesystem::perms::owner_read |
             std::filesystem::perms::owner_write)) {
            return fail("sync preflight did not secure the refresh token");
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(config.sync_directory)) {
            if (entry.path().filename().string().starts_with(
                    ".onedrive-preflight-"
                )) {
                return fail("sync preflight left a probe file behind");
            }
        }
    }

    config.sync_permissions =
        onedrive::config::SyncPermissionsMode::umask;
    config.sync_directory = temporary.path() / "shared-files";
    std::filesystem::create_directory(config.sync_directory);
    std::filesystem::permissions(
        config.sync_directory,
        std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec
    );
    {
        const RuntimePreflight preflight{config, Operation::synchronize};
    }
    if ((std::filesystem::status(config.sync_directory).permissions() &
         std::filesystem::perms::all) !=
        (std::filesystem::perms::owner_all |
         std::filesystem::perms::group_read |
         std::filesystem::perms::group_exec |
         std::filesystem::perms::others_read |
         std::filesystem::perms::others_exec)) {
        return fail("umask mode unexpectedly changed sync directory permissions");
    }

    std::filesystem::remove(token_path);
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config,
                    Operation::synchronize
                };
            },
            "run 'onedrive-cpp auth'"
        )) {
        return fail("sync without a refresh token was accepted");
    }
    return EXIT_SUCCESS;
}

int test_unsafe_paths_and_configuration() {
    using onedrive::app::detail::Operation;
    using onedrive::app::detail::RuntimePreflight;

    TemporaryDirectory temporary;
    auto config = config_for(temporary);

    config.application_id.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config,
                    Operation::authenticate
                };
            },
            "auth.application_id"
        )) {
        return fail("invalid authentication configuration was accepted");
    }
    config.application_id = "test-application";

    config.auth_scope = "Files.ReadWrite offline_access";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config,
                    Operation::authenticate
                };
            },
            "User.Read"
        )) {
        return fail("authentication without User.Read was accepted");
    }
    config.auth_scope = "User.Read Files.ReadWrite offline_access";

    config.sync_directory = config.state_directory / "files";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "must not contain one another"
        )) {
        return fail("overlapping state and sync directories were accepted");
    }

    config.sync_directory = "/";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "must not be the filesystem root"
        )) {
        return fail("filesystem root was accepted as sync.directory");
    }

    config.sync_directory = temporary.path() / "missing-monitor-root";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "requires an existing sync.directory"
        )) {
        return fail("missing monitor directory was accepted");
    }

    const auto real = temporary.path() / "real-sync";
    const auto link = temporary.path() / "linked-sync";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, link);
    config.sync_directory = link;
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "symbolic link"
        )) {
        return fail("symbolic-link sync directory was accepted");
    }

    config.state_directory = temporary.path() / "database-state";
    std::filesystem::create_directory(config.state_directory);
    std::filesystem::permissions(
        config.state_directory,
        std::filesystem::perms::owner_all
    );
    const auto outside_token = temporary.path() / "outside-token";
    {
        std::ofstream output{outside_token};
        output << "token";
    }
    const auto account_directory =
        config.state_directory / "accounts/Test-User--12345678";
    std::filesystem::create_directories(account_directory);
    {
        std::ofstream marker{config.state_directory / "active_account"};
        marker << account_directory.filename().string() << '\n';
    }
    std::filesystem::create_symlink(
        outside_token,
        account_directory / "refresh_token"
    );
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::logout};
            },
            "refresh token"
        )) {
        return fail("symbolic-link refresh token was accepted");
    }

    const auto real_state = temporary.path() / "real-state";
    const auto state_link = temporary.path() / "linked-state";
    std::filesystem::create_directory(real_state);
    std::filesystem::create_directory_symlink(real_state, state_link);
    config.state_directory = state_link;
    config.sync_directory = real;
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::logout};
            },
            "symbolic link"
        )) {
        return fail("symbolic-link state directory was accepted");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_private_state_and_lock();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_sync_directory_and_token();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_unsafe_paths_and_configuration();
}
