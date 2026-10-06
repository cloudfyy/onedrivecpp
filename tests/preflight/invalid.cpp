#include "support.hpp"

#include "onedrive/util/mount.hpp"

namespace {

using namespace onedrive::test::preflight;

int test_invalid() {
    using onedrive::app::detail::Operation;
    using onedrive::app::detail::RuntimePreflight;

    TemporaryDirectory temporary;
    auto config = config_for(temporary);

    config.state_directory.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::logout};
            },
            "state.directory must not be empty"
        )) {
        return fail("empty state directory was accepted");
    }
    config = config_for(temporary);

    config.sync_data_directory.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "sync.data_directory must not be empty"
        )) {
        return fail("empty sync directory was accepted");
    }
    config = config_for(temporary);

    config.drive_id.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::reset_state
                };
            },
            "sync.drive_id must not be empty"
        )) {
        return fail("empty drive identifier was accepted");
    }
    config.drive_id = "me";

    config.application_id.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "auth.application_id"
        )) {
        return fail("invalid authentication configuration was accepted");
    }
    config.application_id = "test-application";

    config.azure_tenant_id.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "auth.application_id"
        )) {
        return fail("empty Azure tenant identifier was accepted");
    }
    config.azure_tenant_id = "common";

    config.auth_scope.clear();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "auth.application_id"
        )) {
        return fail("empty authentication scope was accepted");
    }

    config.auth_scope = "Files.ReadWrite offline_access";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "User.Read"
        )) {
        return fail("authentication without User.Read was accepted");
    }
    config.auth_scope = "User.Read Files.ReadWrite";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "offline_access"
        )) {
        return fail("authentication without offline_access was accepted");
    }
    config.auth_scope = "User.Read Files.ReadWrite offline_access";

    config.auth_endpoint = "http://login.example.test";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::authenticate
                };
            },
            "auth.endpoint"
        )) {
        return fail("insecure authentication endpoint was accepted");
    }
    config.auth_endpoint = "https://login.example.test";

    config.sync_data_directory = config.state_directory / "files";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "must not contain one another"
        )) {
        return fail("overlapping state and sync directories were accepted");
    }

    config.sync_data_directory = "/";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "must not be the filesystem root"
        )) {
        return fail("filesystem root was accepted as sync.data_directory");
    }

    config.sync_data_directory = temporary.path() / "missing-monitor-root";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "requires an existing sync.data_directory"
        )) {
        return fail("missing monitor directory was accepted");
    }

    const auto real = temporary.path() / "real-sync";
    const auto link = temporary.path() / "linked-sync";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, link);
    config.sync_data_directory = link;
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "symbolic link"
        )) {
        return fail("symbolic-link sync directory was accepted");
    }

    config = config_for(temporary);
    std::filesystem::create_directories(config.sync_data_directory);
    config.sync_data_mount_point = "/proc";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "must be inside sync.data_mount_point"
        )) {
        return fail("sync directory outside its mount point was accepted");
    }

    config.sync_data_mount_point = std::filesystem::path{};
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "sync.data_mount_point must not be empty"
        )) {
        return fail("empty runtime sync mount point was accepted");
    }

    config.sync_data_mount_point = temporary.path();
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "not currently mounted"
        )) {
        return fail(
            "ordinary directory was accepted as a mounted sync disk"
        );
    }

    const auto missing_mount = temporary.path() / "missing-mount";
    config.sync_data_mount_point = missing_mount;
    config.sync_data_directory = missing_mount / "OneDrive";
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{config, Operation::monitor};
            },
            "cannot inspect sync.data_mount_point"
        )) {
        return fail("missing sync mount path was accepted");
    }

    const auto mount_file = temporary.path() / "mount-file";
    {
        std::ofstream output{mount_file};
        output << "not a mount";
    }
    config.sync_data_mount_point = mount_file;
    config.sync_data_directory = mount_file / "OneDrive";
    if (!throws_with(
            [&] {
                onedrive::util::require_sync_mount(
                    config.sync_data_directory,
                    config.sync_data_mount_point
                );
            },
            "sync.data_mount_point is not a directory"
        )) {
        return fail("regular file was accepted as a sync mount point");
    }

    config.sync_data_directory = temporary.path() / "files";
    config.sync_data_mount_point = "/";
    {
        const RuntimePreflight preflight{config, Operation::monitor};
    }

    config.state_directory = temporary.path() / "database-state";
    std::filesystem::create_directory(config.state_directory);
    std::filesystem::permissions(
        config.state_directory, std::filesystem::perms::owner_all
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
        outside_token, account_directory / "refresh_token"
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
    config.sync_data_directory = real;
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

} // namespace

int main() {
    return test_invalid();
}
