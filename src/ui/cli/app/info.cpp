#include "info.hpp"
#include "inspection.hpp"
#include "drive_fields.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/app/queries.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include "util/ascii.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::app::detail {
namespace {

std::string delete_policy_name(sync::DeletePolicy policy) {
    return policy == sync::DeletePolicy::propagate ? "propagate" : "preserve";
}

std::string format_timestamp(std::int64_t seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm broken_down{};
    if (::gmtime_r(&value, &broken_down) == nullptr) {
        return "unavailable";
    }
    std::ostringstream output;
    output << std::put_time(&broken_down, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::vector<cli::Field> drive_file_fields(const InspectionStore& inspection) {
    std::set<std::string> known;
    std::set<std::string> downloaded;
    std::set<std::string> pending;
    std::set<std::string> blocked;
    if (inspection.items) {
        const auto& drive_id = inspection.identity.drive_id;
        for (const auto& item : inspection.items->drive_items(drive_id)) {
            if (item.directory) {
                continue;
            }
            known.insert(item.remote_id);
            if (inspect_file(inspection.sync_root, item).actual_size) {
                downloaded.insert(item.remote_id);
            }
        }
        const auto record_pending = [&](const storage::ItemState& item) {
            if (!item.directory) {
                known.insert(item.remote_id);
                pending.insert(item.remote_id);
            }
        };
        for (const auto& download :
             inspection.items->pending_downloads(drive_id)) {
            record_pending(download.item);
        }
        for (const auto& partial :
             inspection.items->partial_downloads(drive_id)) {
            record_pending(partial.item);
        }
        for (const auto& item : inspection.items->blocked_items(drive_id)) {
            if (!item.directory && !item.deleted) {
                known.insert(item.remote_id);
                blocked.insert(item.remote_id);
            }
        }
    }
    const auto count = [&](std::size_t value) {
        return inspection.items ? std::to_string(value) : "unavailable";
    };
    return {
        {
            .label = "statistics source:",
            .key = "statistics_source",
            .value = "local state (not cloud totals)",
        },
        {.label = "known files:",
         .key = "known_files",
         .value = count(known.size())},
        {.label = "downloaded (local):",
         .key = "downloaded_files",
         .value = count(downloaded.size())},
        {.label = "pending downloads:",
         .key = "pending_files",
         .value = count(pending.size())},
        {.label = "blocked files:",
         .key = "blocked_files",
         .value = count(blocked.size())},
        {.label = "state database:",
         .key = "state_database",
         .value = inspection.items ? "present" : "unavailable"},
    };
}

std::string color_mode_name(cli::ColorMode mode) {
    switch (mode) {
        case cli::ColorMode::automatic:
            return "auto";
        case cli::ColorMode::always:
            return "always";
        case cli::ColorMode::never:
            return "never";
    }
    return "unknown";
}

std::string ui_mode_name(cli::UiMode mode) {
    switch (mode) {
        case cli::UiMode::automatic:
            return "auto";
        case cli::UiMode::console:
            return "console";
        case cli::UiMode::tui:
            return "tui";
    }
    return "unknown";
}

std::string theme_name(cli::TuiTheme theme) {
    switch (theme) {
        case cli::TuiTheme::hacker:
            return "hacker";
        case cli::TuiTheme::ocean:
            return "ocean";
        case cli::TuiTheme::amber:
            return "amber";
        case cli::TuiTheme::synthwave:
            return "synthwave";
    }
    return "unknown";
}

std::string transfer_order_name(config::TransferOrder order) {
    switch (order) {
        case config::TransferOrder::default_order:
            return "default";
        case config::TransferOrder::size_ascending:
            return "size_asc";
        case config::TransferOrder::size_descending:
            return "size_dsc";
        case config::TransferOrder::name_ascending:
            return "name_asc";
        case config::TransferOrder::name_descending:
            return "name_dsc";
    }
    return "unknown";
}

} // namespace

int show_drives(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto graph = runtime_factory.create_graph_info_client(config);
    const auto configured = graph->drive_info();
    auto drives = graph->list_drives();
    const auto account_identity = graph->drive_identity();
    const auto references = config.configured_drive_ids();
    std::map<std::string, std::string> configured_references{
        {configured.id, references.front()}
    };
    for (std::size_t index = 1; index < references.size(); ++index) {
        const auto& reference = references[index];
        const auto drive =
            runtime_factory
                .create_graph_info_client(config.for_drive(reference))
                ->drive_info();
        if (!configured_references.emplace(drive.id, reference).second) {
            throw std::runtime_error{
                "multiple configured references resolve to the same Drive"
            };
        }
        if (std::ranges::none_of(drives, [&](const auto& listed) {
                return listed.id == drive.id;
            })) {
            drives.push_back(drive);
        }
    }
    if (std::ranges::none_of(
            drives,
            [&configured](const graph::DriveInfo& drive) {
                return drive.id == configured.id;
            }
        )) {
        drives.push_back(configured);
    }
    for (const auto& drive : drives) {
        const auto reference = configured_references.find(drive.id);
        const bool is_configured = reference != configured_references.end();
        auto identity = account_identity;
        identity.drive_id = drive.id;
        identity.drive_name = drive.name;
        identity.drive_type = drive.type;
        identity.configured_drive_id =
            is_configured ? reference->second : drive.id;
        const auto inspection =
            open_inspection_store(config, runtime_factory, std::move(identity));
        auto fields = drive_fields(drive, is_configured);
        if (is_configured) {
            fields.insert(
                fields.begin() + 2,
                {
                    .label = "reference:",
                    .key = "reference",
                    .value = reference->second,
                }
            );
        }
        std::ranges::move(
            drive_file_fields(inspection), std::back_inserter(fields)
        );
        console.section("drive", "OneDrive drive:", fields);
    }
    return exit_code::success;
}

int show_quota(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    const auto snapshot = load_quota_snapshot(config, runtime_factory);
    const auto& drive = snapshot.drive;
    std::vector<cli::Field> fields{
        {.label = "drive:", .key = "drive", .value = drive.name},
        {.label = "drive id:", .key = "drive_id", .value = drive.id},
    };
    std::ranges::move(quota_fields(drive.quota), std::back_inserter(fields));
    console.section("quota", "OneDrive storage quota:", fields);
    return exit_code::success;
}

int show_status(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    const auto snapshot = load_sync_status_snapshot(config, runtime_factory);
    const auto& identity = snapshot.identity;
    const auto& state = snapshot.state;
    const auto& last_run = snapshot.last_run;
    auto fields = account_fields(identity);
    fields.insert(
        fields.end(),
        {
            {
                .label = "drive id:",
                .key = "drive_id",
                .value = identity.drive_id,
            },
            {
                .label = "mode:",
                .key = "mode",
                .value = std::string{sync::sync_mode_name(snapshot.sync_mode)},
            },
            {
                .label = "delete policy:",
                .key = "delete_policy",
                .value = delete_policy_name(snapshot.delete_policy),
            },
            {
                .label = "last sync:",
                .key = "last_sync",
                .value =
                    last_run
                        ? format_timestamp(last_run->completed_at_unix_seconds)
                        : "never",
            },
            {
                .label = "last result:",
                .key = "last_result",
                .value =
                    !last_run ? "unavailable"
                    : last_run->outcome == metrics::SyncRunOutcome::succeeded
                        ? "succeeded"
                    : last_run->outcome == metrics::SyncRunOutcome::cancelled
                        ? "cancelled"
                        : "failed",
            },
            {
                .label = "tracked:",
                .key = "tracked_items",
                .value = std::to_string(state.tracked_items),
            },
            {
                .label = "blocked:",
                .key = "blocked_items",
                .value = std::to_string(state.blocked_items),
            },
            {
                .label = "pending downloads:",
                .key = "pending_downloads",
                .value = std::to_string(state.pending_downloads),
            },
            {
                .label = "partial downloads:",
                .key = "partial_downloads",
                .value = std::to_string(state.partial_downloads),
            },
            {
                .label = "pending uploads:",
                .key = "pending_uploads",
                .value = std::to_string(state.pending_uploads),
            },
            {
                .label = "pending deletes:",
                .key = "pending_deletes",
                .value = std::to_string(state.pending_deletes),
            },
            {
                .label = "pending remote moves:",
                .key = "pending_remote_moves",
                .value = std::to_string(state.pending_remote_moves),
            },
            {
                .label = "pending local moves:",
                .key = "pending_local_moves",
                .value = std::to_string(state.pending_local_moves),
            },
            {
                .label = "pending delta batches:",
                .key = "pending_delta_batches",
                .value = std::to_string(state.pending_deltas),
            },
            {
                .label = "pending delta changes:",
                .key = "pending_delta_changes",
                .value = std::to_string(state.pending_delta_changes),
            },
            {
                .label = "delta cursor:",
                .key = "delta_cursor",
                .value = state.delta_cursor ? "available" : "absent",
            },
            {
                .label = "filter fingerprint:",
                .key = "sync_filter_fingerprint",
                .value = state.sync_filter_fingerprint.empty()
                             ? "none"
                             : state.sync_filter_fingerprint,
            },
            {
                .label = "state database:",
                .key = "state_database",
                .value = state.database_present ? "available" : "absent",
            },
            {
                .label = "WebSocket:",
                .key = "websocket_enabled",
                .value = snapshot.websocket_enabled ? "enabled" : "disabled",
            },
        }
    );
    console.section("status", "Synchronization status:", fields);
    return exit_code::success;
}

int show_blocked(
    const config::Config& config,
    const cli::Console& console,
    const std::optional<std::string>& reason,
    std::string_view search
) {
    const auto blocked = load_blocked_items(config);
    const auto reason_filter =
        reason ? std::optional<std::string_view>{*reason} : std::nullopt;
    const auto text_matches = [search](const std::string& text) {
        return util::ascii_icontains(text, search);
    };
    std::size_t shown = 0;
    for (const auto& item : blocked) {
        if (!matches_blocked_item(item, reason_filter, text_matches)) {
            continue;
        }
        const auto& strategy = sync::blocked_item_strategy(item.reason_code);
        const auto control = strategy.traits().control;
        const std::string policy_control =
            control == sync::BlockedPolicyControl::local_conflict_configuration
                ? "local_conflict_configuration"
            : control ==
                    sync::BlockedPolicyControl::remote_deletion_configuration
                ? "remote_deletion_configuration"
                : "fixed";
        console.section(
            "blocked_item",
            "Blocked item:",
            {
                {.label = "remote path:",
                 .key = "remote_path",
                 .value = item.remote_path},
                {.label = "reason:",
                 .key = "reason_code",
                 .value = item.reason_code},
                {.label = "detail:",
                 .key = "reason_message",
                 .value = item.reason_message},
                {.label = "Drive ID:",
                 .key = "drive_id",
                 .value = item.drive_id},
                {.label = "item ID:",
                 .key = "remote_id",
                 .value = item.remote_id},
                {.label = "recorded discoveries:",
                 .key = "attempt_count",
                 .value = std::to_string(item.attempt_count)},
                {.label = "policy control:",
                 .key = "policy_control",
                 .value = policy_control},
                {.label = "recovery allowed:",
                 .key = "recovery_allowed",
                 .value = strategy.allows_recovery() ? "true" : "false"},
            }
        );
        ++shown;
    }
    if (shown == 0) {
        console.message(
            cli::MessageKind::information,
            "no_blocked_items",
            blocked.empty() ? "No saved blocked items."
                            : "No saved blocked items match the filters."
        );
    }
    console.section(
        "blocked_item_summary",
        "Blocked item summary:",
        {
            {.label = "recorded:",
             .key = "recorded",
             .value = std::to_string(blocked.size())},
            {.label = "shown:", .key = "shown", .value = std::to_string(shown)},
        }
    );
    return exit_code::success;
}

int show_config(
    const config::Config& config,
    const std::filesystem::path& config_path,
    const cli::Console& console
) {
    console.section(
        "effective_config",
        "Effective configuration:",
        {
            {
                .label = "config file:",
                .key = "config_file",
                .value = config_path.string(),
            },
            {
                .label = "data directory:",
                .key = "data_directory",
                .value = config.sync_data_directory.string(),
            },
            {
                .label = "state directory:",
                .key = "state_directory",
                .value = config.state_directory.string(),
            },
            {
                .label = "data mount point:",
                .key = "data_mount_point",
                .value = config.sync_data_mount_point
                             ? config.sync_data_mount_point->string()
                             : "none",
            },
            {
                .label = "configured drive:",
                .key = "drive_id",
                .value =
                    config.drive_ids.size() > 1 ? "multiple" : config.drive_id,
            },
            {
                .label = "sync list:",
                .key = "sync_list",
                .value = config.sync_list ? config.sync_list->string() : "none",
            },
            {
                .label = "sync mode:",
                .key = "sync_mode",
                .value = std::string{sync::sync_mode_name(config.sync_mode)},
            },
            {
                .label = "delete policy:",
                .key = "delete_policy",
                .value = delete_policy_name(config.delete_policy),
            },
            {
                .label = "dry run:",
                .key = "dry_run",
                .value = config.dry_run ? "true" : "false",
            },
        }
    );
    if (config.drive_ids.size() > 1) {
        for (const auto& id : config.drive_ids) {
            console.section(
                "configured_drive",
                "Configured synchronization Drive:",
                {{.label = "drive:", .key = "drive_id", .value = id}}
            );
        }
    }
    console.section(
        "effective_transfer_config",
        "Effective transfer configuration:",
        {
            {
                .label = "order:",
                .key = "order",
                .value = transfer_order_name(config.transfer_order),
            },
            {
                .label = "download concurrency:",
                .key = "download_concurrency",
                .value = std::to_string(config.download_concurrency),
            },
            {
                .label = "download retries:",
                .key = "download_maximum_retries",
                .value = std::to_string(config.download_maximum_retries),
            },
            {
                .label = "download rate:",
                .key = "download_rate",
                .value = format_bytes(
                    config.download_maximum_rate_bytes_per_second
                ) + "/s",
            },
            {
                .label = "total download rate:",
                .key = "download_total_rate",
                .value = format_bytes(
                    config.download_maximum_total_rate_bytes_per_second
                ) + "/s",
            },
            {
                .label = "upload concurrency:",
                .key = "upload_concurrency",
                .value = std::to_string(config.upload_concurrency),
            },
            {
                .label = "upload rate:",
                .key = "upload_rate",
                .value = format_bytes(
                    config.upload_maximum_rate_bytes_per_second
                ) + "/s",
            },
            {
                .label = "total upload rate:",
                .key = "upload_total_rate",
                .value = format_bytes(
                    config.upload_maximum_total_rate_bytes_per_second
                ) + "/s",
            },
        }
    );
    console.section(
        "effective_output_config",
        "Effective output configuration:",
        {
            {
                .label = "UI:",
                .key = "ui",
                .value = ui_mode_name(config.console_ui),
            },
            {
                .label = "theme:",
                .key = "theme",
                .value = theme_name(config.console_theme),
            },
            {
                .label = "color:",
                .key = "color",
                .value = color_mode_name(config.console_color),
            },
            {
                .label = "file history limit:",
                .key = "file_history_limit",
                .value = std::to_string(config.file_history_limit),
            },
            {
                .label = "log level:",
                .key = "log_level",
                .value = config.logging.level,
            },
            {
                .label = "log file:",
                .key = "log_file",
                .value = config.logging.file ? config.logging.file->string()
                                             : "none",
            },
            {
                .label = "proxy:",
                .key = "proxy",
                .value = config.proxy.url ? "configured" : "disabled",
            },
        }
    );
    return exit_code::success;
}

} // namespace onedrive::app::detail
