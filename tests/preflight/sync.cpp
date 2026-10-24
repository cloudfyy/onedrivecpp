#include "support/progress.hpp"
#include "support.hpp"
#include "app/checks.hpp"
#include "app/lock.hpp"
#include "onedrive/ui/common/observer.hpp"

namespace {

using namespace onedrive::test::preflight;

class Observer final : public onedrive::events::Observer {
    void on_event(const onedrive::events::Event&) const override {
    }
};

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
        const auto resolved = onedrive::config::validate_sync_state_directories(
            config.sync_data_directory / ".", config.state_directory
        );
        if (resolved != config.sync_data_directory) {
            return fail(
                "directory validation did not return the canonical sync path"
            );
        }
        if (std::filesystem::exists(config.sync_data_directory)) {
            return fail(
                "sync preflight created the sync directory before claiming it"
            );
        }
        const Observer observer;
        const auto sync_lock =
            onedrive::app::detail::acquire_sync_directory_lock(
                config.sync_data_directory,
                config.state_directory,
                "synchronize",
                observer
            );
        onedrive::app::detail::prepare_sync_directory(
            config, Operation::synchronize
        );
        const auto sync_permissions =
            std::filesystem::status(config.sync_data_directory).permissions();
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
             std::filesystem::directory_iterator(config.sync_data_directory)) {
            if (entry.path().filename().string().starts_with(
                    ".onedrive-preflight-"
                )) {
                return fail("sync preflight left a probe file behind");
            }
        }
    }

    config.sync_permissions = onedrive::config::SyncPermissionsMode::umask;
    config.sync_data_directory = temporary.path() / "shared-files";
    std::filesystem::create_directory(config.sync_data_directory);
    std::filesystem::permissions(
        config.sync_data_directory,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec
    );
    {
        const RuntimePreflight preflight{config, Operation::synchronize};
        const Observer observer;
        const auto sync_lock =
            onedrive::app::detail::acquire_sync_directory_lock(
                config.sync_data_directory,
                config.state_directory,
                "synchronize",
                observer
            );
        onedrive::app::detail::prepare_sync_directory(
            config, Operation::synchronize
        );
    }
    if ((std::filesystem::status(config.sync_data_directory).permissions() &
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
            "run 'onedrive-cpp account login'"
        )) {
        return fail("sync without a refresh token was accepted");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{1};
    return progress.finish(progress.run("sync", test_sync));
}
