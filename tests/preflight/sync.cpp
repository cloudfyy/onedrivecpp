#include "support.hpp"

namespace {

using namespace onedrive::test::preflight;

int test_sync() {
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

    config.sync_permissions = onedrive::config::SyncPermissionsMode::umask;
    config.sync_directory = temporary.path() / "shared-files";
    std::filesystem::create_directory(config.sync_directory);
    std::filesystem::permissions(
        config.sync_directory,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
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
        return fail(
            "umask mode unexpectedly changed sync directory permissions"
        );
    }

    std::filesystem::remove(token_path);
    if (!throws_with(
            [&] {
                const RuntimePreflight preflight{
                    config, Operation::synchronize
                };
            },
            "run 'onedrive-cpp auth'"
        )) {
        return fail("sync without a refresh token was accepted");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_sync();
}
