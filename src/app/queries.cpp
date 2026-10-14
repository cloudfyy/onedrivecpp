#include "onedrive/app/queries.hpp"

#include "onedrive/app/factory.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/util/path_security.hpp"
#include "util/private_file.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>

namespace onedrive::app {
namespace {

nlohmann::json read_account_metadata(const std::filesystem::path& path) {
    try {
        return nlohmann::json::parse(
            util::read_private_file(
                path,
                "account metadata",
                {.required_owner = ::geteuid(),
                 .exact_permissions = 0600,
                 .permission_requirement =
                     "must be readable and writable only by the current user",
                 .require_single_link = true}
            )
        );
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error{
            "invalid account metadata '" + path.string() + "': " + error.what()
        };
    }
}

} // namespace

std::vector<storage::BlockedItem>
load_blocked_items(const config::Config& config) {
    const auto active = account::AccountState::find_active_token_directory(
        config.state_directory
    );
    if (!active) {
        return {};
    }
    try {
        const auto user = read_account_metadata(*active / "account.json");
        const auto drives = *active / "drives";
        util::reject_symlink_components(drives, "account drive directory");
        std::optional<account::DriveIdentity> selected;
        std::filesystem::path selected_directory;
        for (const auto& entry : std::filesystem::directory_iterator(drives)) {
            if (entry.is_symlink()) {
                throw std::runtime_error{
                    "account drive directory is a symbolic link: " +
                    entry.path().string()
                };
            }
            if (!entry.is_directory()) {
                continue;
            }
            const auto drive =
                read_account_metadata(entry.path() / "drive.json");
            if (drive.at("configured_drive_id").get<std::string>() !=
                    config.drive_id &&
                drive.at("drive_id").get<std::string>() != config.drive_id) {
                continue;
            }
            if (selected) {
                throw std::runtime_error{
                    "multiple saved drives match the configured drive"
                };
            }
            selected = account::DriveIdentity{
                .user_id = user.at("user_id").get<std::string>(),
                .user_display_name = user.at("display_name").get<std::string>(),
                .configured_drive_id =
                    drive.at("configured_drive_id").get<std::string>(),
                .drive_id = drive.at("drive_id").get<std::string>(),
                .drive_name = drive.at("name").get<std::string>(),
                .photo = {},
            };
            selected_directory = entry.path();
        }
        if (!selected) {
            return {};
        }
        const auto database_path = selected_directory / "items.sqlite3";
        util::reject_symlink_components(database_path, "state database");
        if (!std::filesystem::exists(database_path)) {
            return {};
        }
        storage::ItemDatabase database{selected_directory, *selected};
        database.open_read_only();
        return database.blocked_items(selected->drive_id);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error{
            "invalid saved account/drive metadata: " + std::string{error.what()}
        };
    }
}

QuotaSnapshot load_quota_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    return {
        .drive = runtime_factory.create_graph_info_client(config)->drive_info(),
    };
}

SyncStatusSnapshot load_sync_status_snapshot(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    auto graph = runtime_factory.create_graph_info_client(config);
    auto identity = graph->drive_identity();
    const auto paths =
        account::AccountState::locate(config.state_directory, identity);
    const auto drive_id = identity.drive_id;
    return {
        .identity = std::move(identity),
        .sync_mode = config.sync_mode,
        .delete_policy = config.delete_policy,
        .last_run = metrics::load_sync_run_status(paths.drive_directory),
        .state = storage::read_state_summary(paths.drive_directory, drive_id),
        .websocket_enabled = config.monitor_websocket_enabled,
    };
}

} // namespace onedrive::app
