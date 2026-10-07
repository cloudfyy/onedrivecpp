#include "info.hpp"
#include "drive_fields.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/status.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>

namespace onedrive::app::detail {
namespace {

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t unit = 1024;
    constexpr std::array<std::string_view, 5> names{
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= static_cast<double>(unit) && index + 1 < names.size()) {
        value /= static_cast<double>(unit);
        ++index;
    }
    return index == 0 ? std::format("{} {}", bytes, names[index])
                      : std::format("{:.2f} {}", value, names[index]);
}

std::string sync_mode_name(sync::SyncMode mode) {
    switch (mode) {
    case sync::SyncMode::bidirectional:
        return "bidirectional";
    case sync::SyncMode::upload_only:
        return "upload_only";
    case sync::SyncMode::download_only:
        return "download_only";
    }
    return "unknown";
}

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

std::string quota_value(
    const std::optional<graph::DriveQuota>& quota,
    std::uint64_t graph::DriveQuota::* member
) {
    return quota ? format_bytes((*quota).*member) : "unavailable";
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
    if (std::ranges::none_of(
            drives,
            [&configured](const graph::DriveInfo& drive) {
                return drive.id == configured.id;
            }
        )) {
        drives.push_back(configured);
    }
    for (const auto& drive : drives) {
        console.section(
            "drive",
            "OneDrive drive:",
            drive_fields(drive, drive.id == configured.id)
        );
    }
    return 0;
}

int show_quota(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    const auto drive =
        runtime_factory.create_graph_info_client(config)->drive_info();
    console.section(
        "quota",
        "OneDrive storage quota:",
        {
            {.label = "drive:", .key = "drive", .value = drive.name},
            {.label = "drive id:", .key = "drive_id", .value = drive.id},
            {
                .label = "total:",
                .key = "total",
                .value = quota_value(drive.quota, &graph::DriveQuota::total),
            },
            {
                .label = "used:",
                .key = "used",
                .value = quota_value(drive.quota, &graph::DriveQuota::used),
            },
            {
                .label = "remaining:",
                .key = "remaining",
                .value =
                    quota_value(drive.quota, &graph::DriveQuota::remaining),
            },
            {
                .label = "deleted:",
                .key = "deleted",
                .value = quota_value(drive.quota, &graph::DriveQuota::deleted),
            },
            {
                .label = "state:",
                .key = "state",
                .value = drive.quota ? drive.quota->state : "unavailable",
            },
        }
    );
    return 0;
}

int show_status(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto graph = runtime_factory.create_graph_info_client(config);
    const auto identity = graph->drive_identity();
    const auto paths =
        account::AccountState::locate(config.state_directory, identity);
    const auto state =
        storage::read_state_summary(paths.drive_directory, identity.drive_id);
    const auto last_run = metrics::load_sync_run_status(paths.drive_directory);
    console.section(
        "status",
        "Synchronization status:",
        {
            {
                .label = "account:",
                .key = "account",
                .value = identity.user_display_name,
            },
            {.label = "drive:", .key = "drive", .value = identity.drive_name},
            {
                .label = "drive id:",
                .key = "drive_id",
                .value = identity.drive_id,
            },
            {
                .label = "mode:",
                .key = "mode",
                .value = sync_mode_name(config.sync_mode),
            },
            {
                .label = "delete policy:",
                .key = "delete_policy",
                .value = delete_policy_name(config.delete_policy),
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
                .value =
                    config.monitor_websocket_enabled ? "enabled" : "disabled",
            },
        }
    );
    return 0;
}

} // namespace onedrive::app::detail
