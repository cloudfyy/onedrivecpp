#include "info.hpp"
#include "drive_fields.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/storage/status.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "sync/download/integrity.hpp"
#include "sync/filesystem/operations.hpp"
#include "util/ascii.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

struct InspectionStore {
    account::DriveIdentity identity;
    std::filesystem::path sync_root;
    std::filesystem::path database_path;
    std::unique_ptr<storage::ItemStore> items;
};

InspectionStore open_inspection_store(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    auto graph = runtime_factory.create_graph_info_client(config);
    auto identity = graph->drive_identity();
    const auto account_paths =
        account::AccountState::locate(config.state_directory, identity);
    const auto database_path = account_paths.drive_directory / "items.sqlite3";
    std::error_code error;
    const auto database_status =
        std::filesystem::symlink_status(database_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect synchronization state database '" +
            database_path.string() + "': " + error.message()
        );
    }

    std::unique_ptr<storage::ItemStore> items;
    if (std::filesystem::exists(database_status)) {
        if (!std::filesystem::is_regular_file(database_status)) {
            throw std::runtime_error(
                "synchronization state database is not a regular file: " +
                database_path.string()
            );
        }
        items = runtime_factory.create_item_store(config, identity);
        items->open_read_only();
    }
    auto sync_root = account::AccountState::drive_data_directory(
        config.sync_data_directory, identity
    );
    return {
        .identity = std::move(identity),
        .sync_root = std::move(sync_root),
        .database_path = database_path,
        .items = std::move(items),
    };
}

bool path_is_within(
    const std::filesystem::path& root, const std::filesystem::path& path
) {
    const auto relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

std::uint64_t checked_add(std::uint64_t total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::runtime_error("local synchronization byte count overflow");
    }
    return total + value;
}

enum class LocalFileStatus {
    ok,
    missing,
    modified,
    type_changed,
    outside_root,
};

std::string_view status_name(LocalFileStatus status) {
    switch (status) {
        case LocalFileStatus::ok:
            return "ok";
        case LocalFileStatus::missing:
            return "missing";
        case LocalFileStatus::modified:
            return "modified";
        case LocalFileStatus::type_changed:
            return "type-changed";
        case LocalFileStatus::outside_root:
            return "outside-root";
    }
    return "unknown";
}

struct InspectedFile {
    const storage::ItemState* item;
    LocalFileStatus status;
    std::optional<std::uint64_t> actual_size;
};

InspectedFile inspect_file(
    const std::filesystem::path& root, const storage::ItemState& item
) {
    if (!path_is_within(root, item.local_path)) {
        return {
            .item = &item,
            .status = LocalFileStatus::outside_root,
            .actual_size = std::nullopt,
        };
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(item.local_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {
            .item = &item,
            .status = LocalFileStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect downloaded file '" + item.local_path.string() +
            "': " + error.message()
        );
    }
    if (!std::filesystem::exists(status)) {
        return {
            .item = &item,
            .status = LocalFileStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {
            .item = &item,
            .status = LocalFileStatus::type_changed,
            .actual_size = std::nullopt,
        };
    }
    const auto actual_size = std::filesystem::file_size(item.local_path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read downloaded file size '" + item.local_path.string() +
            "': " + error.message()
        );
    }
    return {
        .item = &item,
        .status =
            sync::detail::local_snapshot_matches(item, item.local_path)
                ? LocalFileStatus::ok
                : LocalFileStatus::modified,
        .actual_size = static_cast<std::uint64_t>(actual_size),
    };
}

std::string normalized_inspection_path(std::string_view path) {
    std::string result{path};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    if (result.empty()) {
        return {};
    }
    if (result.front() == '/') {
        throw std::invalid_argument(
            "inspect files PATH must be Drive-relative"
        );
    }
    std::size_t begin = 0;
    while (begin <= result.size()) {
        const auto end = result.find('/', begin);
        const auto component = std::string_view{result}.substr(
            begin,
            end == std::string::npos ? result.size() - begin : end - begin
        );
        if (component.empty() || component == "." || component == "..") {
            throw std::invalid_argument(
                "inspect files PATH contains an invalid component"
            );
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

bool path_matches_filter(std::string_view path, std::string_view filter) {
    return filter.empty() || path == filter ||
           (path.size() > filter.size() && path.starts_with(filter) &&
            path[filter.size()] == '/');
}

std::string hash_algorithm_name(util::FileHashAlgorithm algorithm) {
    return algorithm == util::FileHashAlgorithm::sha256 ? "sha256"
                                                        : "quick_xor";
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

bool partial_filename(std::string_view name) {
    return name.starts_with('.') &&
           name.find(".onedrive-partial-") != std::string_view::npos;
}

enum class PartialStatus {
    resumable,
    missing,
    type_changed,
    outside_root,
    path_mismatch,
    invalid_checkpoint,
};

std::string_view status_name(PartialStatus status) {
    switch (status) {
        case PartialStatus::resumable:
            return "resumable";
        case PartialStatus::missing:
            return "missing";
        case PartialStatus::type_changed:
            return "type-changed";
        case PartialStatus::outside_root:
            return "outside-root";
        case PartialStatus::path_mismatch:
            return "path-mismatch";
        case PartialStatus::invalid_checkpoint:
            return "invalid-checkpoint";
    }
    return "unknown";
}

struct InspectedPartial {
    const storage::PartialDownload* partial;
    PartialStatus status;
    std::optional<std::uint64_t> actual_size;
};

InspectedPartial inspect_partial(
    const std::filesystem::path& root,
    const storage::PartialDownload& partial
) {
    if (!path_is_within(root, partial.item.local_path)) {
        return {
            .partial = &partial,
            .status = PartialStatus::outside_root,
            .actual_size = std::nullopt,
        };
    }
    if (!sync::detail::is_temporary_path_for(
            partial.item.local_path, partial.temporary_path
        )) {
        return {
            .partial = &partial,
            .status = PartialStatus::path_mismatch,
            .actual_size = std::nullopt,
        };
    }
    if (partial.item.size < 0 ||
        partial.completed_bytes >
            static_cast<std::uint64_t>(partial.item.size)) {
        return {
            .partial = &partial,
            .status = PartialStatus::invalid_checkpoint,
            .actual_size = std::nullopt,
        };
    }
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(partial.temporary_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {
            .partial = &partial,
            .status = PartialStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect partial download '" +
            partial.temporary_path.string() + "': " + error.message()
        );
    }
    if (!std::filesystem::exists(status)) {
        return {
            .partial = &partial,
            .status = PartialStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {
            .partial = &partial,
            .status = PartialStatus::type_changed,
            .actual_size = std::nullopt,
        };
    }
    const auto actual_size =
        std::filesystem::file_size(partial.temporary_path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read partial download size '" +
            partial.temporary_path.string() + "': " + error.message()
        );
    }
    return {
        .partial = &partial,
        .status = actual_size < partial.completed_bytes
                    ? PartialStatus::invalid_checkpoint
                    : PartialStatus::resumable,
        .actual_size = static_cast<std::uint64_t>(actual_size),
    };
}

std::filesystem::space_info filesystem_space(
    const std::filesystem::path& requested_path
) {
    auto probe = requested_path;
    std::error_code error;
    while (true) {
        const auto status = std::filesystem::symlink_status(probe, error);
        if (!error && std::filesystem::exists(status)) {
            break;
        }
        if (error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "cannot inspect storage path '" + probe.string() + "': " +
                error.message()
            );
        }
        error.clear();
        const auto parent = probe.parent_path();
        if (parent.empty() || parent == probe) {
            throw std::runtime_error(
                "cannot find an existing filesystem ancestor for '" +
                requested_path.string() + "'"
            );
        }
        probe = parent;
    }
    const auto result = std::filesystem::space(probe, error);
    if (error) {
        throw std::runtime_error(
            "cannot read filesystem space for '" + probe.string() + "': " +
            error.message()
        );
    }
    return result;
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

int show_storage(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto inspection = open_inspection_store(config, runtime_factory);
    std::vector<storage::ItemState> items;
    std::vector<storage::PartialDownload> partials;
    if (inspection.items) {
        items = inspection.items->drive_items(inspection.identity.drive_id);
        partials =
            inspection.items->partial_downloads(inspection.identity.drive_id);
    }

    std::uint64_t tracked_bytes = 0;
    std::size_t tracked_files = 0;
    for (const auto& item : items) {
        if (item.directory) {
            continue;
        }
        ++tracked_files;
        if (item.local_size >= 0) {
            tracked_bytes = checked_add(
                tracked_bytes, static_cast<std::uint64_t>(item.local_size)
            );
        }
    }

    std::uint64_t partial_bytes = 0;
    std::size_t resumable_partials = 0;
    for (const auto& partial : partials) {
        const auto inspected = inspect_partial(inspection.sync_root, partial);
        if (inspected.actual_size) {
            partial_bytes =
                checked_add(partial_bytes, *inspected.actual_size);
        }
        resumable_partials +=
            inspected.status == PartialStatus::resumable ? 1U : 0U;
    }

    std::error_code root_error;
    const bool root_exists = std::filesystem::is_directory(
        std::filesystem::symlink_status(inspection.sync_root, root_error)
    );
    if (root_error == std::errc::no_such_file_or_directory) {
        root_error.clear();
    } else if (root_error) {
        throw std::runtime_error(
            "cannot inspect synchronization root '" +
            inspection.sync_root.string() + "': " + root_error.message()
        );
    }
    const auto space = filesystem_space(inspection.sync_root);
    console.section(
        "storage",
        "Local synchronization storage:",
        {
            {
                .label = "root:",
                .key = "root",
                .value = inspection.sync_root.string(),
            },
            {
                .label = "root exists:",
                .key = "root_exists",
                .value = root_exists ? "true" : "false",
            },
            {
                .label = "filesystem capacity:",
                .key = "filesystem_capacity",
                .value = format_bytes(space.capacity),
            },
            {
                .label = "filesystem available:",
                .key = "filesystem_available",
                .value = format_bytes(space.available),
            },
            {
                .label = "tracked files:",
                .key = "tracked_files",
                .value = std::to_string(tracked_files),
            },
            {
                .label = "tracked bytes:",
                .key = "tracked_bytes",
                .value = format_bytes(tracked_bytes),
            },
            {
                .label = "partial downloads:",
                .key = "partial_downloads",
                .value = std::to_string(partials.size()),
            },
            {
                .label = "resumable partials:",
                .key = "resumable_partials",
                .value = std::to_string(resumable_partials),
            },
            {
                .label = "partial bytes:",
                .key = "partial_bytes",
                .value = format_bytes(partial_bytes),
            },
            {
                .label = "state database:",
                .key = "state_database",
                .value = inspection.items ? inspection.database_path.string()
                                          : "absent",
            },
        }
    );
    return 0;
}

int show_partials(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto partials =
        inspection.items
            ? inspection.items->partial_downloads(inspection.identity.drive_id)
            : std::vector<storage::PartialDownload>{};
    std::size_t resumable = 0;
    std::uint64_t actual_bytes = 0;
    for (const auto& partial : partials) {
        const auto inspected = inspect_partial(inspection.sync_root, partial);
        resumable += inspected.status == PartialStatus::resumable ? 1U : 0U;
        if (inspected.actual_size) {
            actual_bytes =
                checked_add(actual_bytes, *inspected.actual_size);
        }
        console.section(
            "partial_download",
            "Partial download:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = partial.item.remote_path,
                },
                {
                    .label = "destination:",
                    .key = "destination",
                    .value = partial.item.local_path.string(),
                },
                {
                    .label = "temporary file:",
                    .key = "temporary_path",
                    .value = partial.temporary_path.string(),
                },
                {
                    .label = "status:",
                    .key = "status",
                    .value = std::string{status_name(inspected.status)},
                },
                {
                    .label = "completed:",
                    .key = "completed_bytes",
                    .value = format_bytes(partial.completed_bytes),
                },
                {
                    .label = "expected:",
                    .key = "expected_bytes",
                    .value = partial.item.size < 0
                               ? "invalid"
                               : format_bytes(static_cast<std::uint64_t>(
                                     partial.item.size
                                 )),
                },
                {
                    .label = "actual:",
                    .key = "actual_bytes",
                    .value = inspected.actual_size
                               ? format_bytes(*inspected.actual_size)
                               : "unavailable",
                },
            }
        );
    }
    if (partials.empty()) {
        console.message(
            cli::MessageKind::information,
            "no_partial_downloads",
            "No partial downloads are recorded."
        );
    }
    console.section(
        "partial_download_summary",
        "Partial download summary:",
        {
            {
                .label = "recorded:",
                .key = "recorded",
                .value = std::to_string(partials.size()),
            },
            {
                .label = "resumable:",
                .key = "resumable",
                .value = std::to_string(resumable),
            },
            {
                .label = "invalid:",
                .key = "invalid",
                .value = std::to_string(partials.size() - resumable),
            },
            {
                .label = "actual bytes:",
                .key = "actual_bytes",
                .value = format_bytes(actual_bytes),
            },
        }
    );
    return 0;
}

int show_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    const std::optional<std::string>& status
) {
    const auto filter = normalized_inspection_path(path);
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto items =
        inspection.items
            ? inspection.items->drive_items(inspection.identity.drive_id)
            : std::vector<storage::ItemState>{};
    std::array<std::size_t, 5> counts{};
    std::size_t inspected_count = 0;
    std::size_t shown = 0;
    for (const auto& item : items) {
        if (item.directory ||
            !path_matches_filter(item.remote_path, filter)) {
            continue;
        }
        const auto inspected = inspect_file(inspection.sync_root, item);
        ++inspected_count;
        ++counts[static_cast<std::size_t>(inspected.status)];
        if (status && *status != status_name(inspected.status)) {
            continue;
        }
        ++shown;
        console.section(
            "downloaded_file",
            "Downloaded file:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = item.remote_path,
                },
                {
                    .label = "local path:",
                    .key = "local_path",
                    .value = item.local_path.string(),
                },
                {
                    .label = "status:",
                    .key = "status",
                    .value = std::string{status_name(inspected.status)},
                },
                {
                    .label = "expected size:",
                    .key = "expected_size",
                    .value = item.local_size < 0
                               ? "invalid"
                               : format_bytes(
                                     static_cast<std::uint64_t>(item.local_size)
                                 ),
                },
                {
                    .label = "actual size:",
                    .key = "actual_size",
                    .value = inspected.actual_size
                               ? format_bytes(*inspected.actual_size)
                               : "unavailable",
                },
            }
        );
    }
    if (shown == 0) {
        console.message(
            cli::MessageKind::information,
            "no_downloaded_files",
            "No downloaded files matched the requested filters."
        );
    }
    console.section(
        "downloaded_file_summary",
        "Downloaded file summary:",
        {
            {
                .label = "inspected:",
                .key = "inspected",
                .value = std::to_string(inspected_count),
            },
            {
                .label = "shown:",
                .key = "shown",
                .value = std::to_string(shown),
            },
            {
                .label = "ok:",
                .key = "ok",
                .value =
                    std::to_string(counts[static_cast<std::size_t>(
                        LocalFileStatus::ok
                    )]),
            },
            {
                .label = "missing:",
                .key = "missing",
                .value =
                    std::to_string(counts[static_cast<std::size_t>(
                        LocalFileStatus::missing
                    )]),
            },
            {
                .label = "modified:",
                .key = "modified",
                .value =
                    std::to_string(counts[static_cast<std::size_t>(
                        LocalFileStatus::modified
                    )]),
            },
            {
                .label = "type changed:",
                .key = "type_changed",
                .value =
                    std::to_string(counts[static_cast<std::size_t>(
                        LocalFileStatus::type_changed
                    )]),
            },
            {
                .label = "outside root:",
                .key = "outside_root",
                .value =
                    std::to_string(counts[static_cast<std::size_t>(
                        LocalFileStatus::outside_root
                    )]),
            },
        }
    );
    return 0;
}

int verify_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    std::string_view mode
) {
    const auto filter = normalized_inspection_path(path);
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto items =
        inspection.items
            ? inspection.items->drive_items(inspection.identity.drive_id)
            : std::vector<storage::ItemState>{};
    std::size_t verified = 0;
    std::size_t failed = 0;
    std::size_t unavailable = 0;
    for (const auto& item : items) {
        if (item.directory ||
            !path_matches_filter(item.remote_path, filter)) {
            continue;
        }
        const auto metadata = inspect_file(inspection.sync_root, item);
        std::string result{status_name(metadata.status)};
        std::string algorithm{"none"};
        if (metadata.status == LocalFileStatus::ok) {
            if (mode == "metadata") {
                result = "verified";
                ++verified;
            } else if (!item.content_hash) {
                result = "hash-unavailable";
                ++unavailable;
            } else {
                algorithm = hash_algorithm_name(item.content_hash->algorithm);
                const auto actual =
                    item.content_hash->algorithm ==
                            util::FileHashAlgorithm::sha256
                        ? sync::detail::content_fingerprint(item.local_path)
                        : sync::detail::quick_xor_hash(item.local_path);
                const bool matches =
                    item.content_hash->algorithm ==
                            util::FileHashAlgorithm::sha256
                        ? util::ascii_iequals(actual, item.content_hash->value)
                        : actual == item.content_hash->value;
                result = matches ? "verified" : "hash-mismatch";
                matches ? ++verified : ++failed;
            }
        } else {
            ++failed;
        }
        console.section(
            "file_verification",
            "File verification:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = item.remote_path,
                },
                {
                    .label = "local path:",
                    .key = "local_path",
                    .value = item.local_path.string(),
                },
                {.label = "mode:", .key = "mode", .value = std::string{mode}},
                {.label = "result:", .key = "result", .value = result},
                {
                    .label = "algorithm:",
                    .key = "algorithm",
                    .value = algorithm,
                },
            }
        );
    }
    console.section(
        "file_verification_summary",
        "File verification summary:",
        {
            {
                .label = "verified:",
                .key = "verified",
                .value = std::to_string(verified),
            },
            {
                .label = "failed:",
                .key = "failed",
                .value = std::to_string(failed),
            },
            {
                .label = "hash unavailable:",
                .key = "hash_unavailable",
                .value = std::to_string(unavailable),
            },
        }
    );
    return failed == 0 && unavailable == 0 ? 0 : 1;
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
                .value = config.drive_id,
            },
            {
                .label = "sync list:",
                .key = "sync_list",
                .value = config.sync_list ? config.sync_list->string() : "none",
            },
            {
                .label = "sync mode:",
                .key = "sync_mode",
                .value = sync_mode_name(config.sync_mode),
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
    return 0;
}

int cleanup_state(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    bool dry_run,
    bool assume_yes
) {
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto partials =
        inspection.items
            ? inspection.items->partial_downloads(inspection.identity.drive_id)
            : std::vector<storage::PartialDownload>{};
    std::vector<const storage::PartialDownload*> invalid;
    std::set<std::filesystem::path> recorded;
    for (const auto& partial : partials) {
        recorded.insert(partial.temporary_path.lexically_normal());
        if (inspect_partial(inspection.sync_root, partial).status !=
            PartialStatus::resumable) {
            invalid.push_back(&partial);
        }
    }

    std::vector<std::filesystem::path> orphaned;
    std::error_code root_error;
    if (std::filesystem::is_directory(inspection.sync_root, root_error)) {
        std::filesystem::recursive_directory_iterator iterator{
            inspection.sync_root,
            std::filesystem::directory_options::skip_permission_denied
        };
        for (const auto& entry : iterator) {
            std::error_code status_error;
            const auto status = entry.symlink_status(status_error);
            if (status_error || !std::filesystem::is_regular_file(status) ||
                !partial_filename(entry.path().filename().string()) ||
                recorded.contains(entry.path().lexically_normal())) {
                continue;
            }
            orphaned.push_back(entry.path());
        }
    } else if (root_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            "cannot inspect synchronization root for cleanup: " +
            root_error.message()
        );
    }

    for (const auto* partial : invalid) {
        console.section(
            "cleanup_partial",
            "Invalid partial download:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = partial->item.remote_path,
                },
                {
                    .label = "temporary path:",
                    .key = "temporary_path",
                    .value = partial->temporary_path.string(),
                },
                {
                    .label = "status:",
                    .key = "status",
                    .value = std::string{status_name(
                        inspect_partial(inspection.sync_root, *partial).status
                    )},
                },
            }
        );
    }
    for (const auto& path : orphaned) {
        console.section(
            "cleanup_orphan",
            "Orphaned partial file:",
            {
                {
                    .label = "path:",
                    .key = "path",
                    .value = path.string(),
                },
            }
        );
    }
    console.section(
        "cleanup_summary",
        "Partial cleanup summary:",
        {
            {
                .label = "invalid records:",
                .key = "invalid_records",
                .value = std::to_string(invalid.size()),
            },
            {
                .label = "orphaned files:",
                .key = "orphaned_files",
                .value = std::to_string(orphaned.size()),
            },
            {
                .label = "mode:",
                .key = "mode",
                .value = dry_run ? "dry-run" : "cleanup",
            },
        }
    );
    if (dry_run || (invalid.empty() && orphaned.empty())) {
        return 0;
    }
    if (!assume_yes) {
        if (console.output_mode() == cli::OutputMode::json) {
            console.message(
                cli::MessageKind::error,
                "confirmation_required",
                "Cleanup requires --yes with JSON output."
            );
            return 1;
        }
        if (!console.confirm(
                "cleanup_confirmation",
                "Type the configured drive reference '" +
                    inspection.identity.configured_drive_id +
                    "' to confirm cleanup: ",
                inspection.identity.configured_drive_id
            )) {
            console.message(
                cli::MessageKind::warning,
                "cleanup_cancelled",
                "Partial cleanup cancelled."
            );
            return 1;
        }
    }

    inspection.items.reset();
    auto writable =
        runtime_factory.create_item_store(config, inspection.identity);
    writable->open();
    std::size_t removed_files = 0;
    for (const auto* partial : invalid) {
        const auto inspected = inspect_partial(inspection.sync_root, *partial);
        if (inspected.actual_size &&
            path_is_within(inspection.sync_root, partial->temporary_path) &&
            sync::detail::is_temporary_path_for(
                partial->item.local_path, partial->temporary_path
            ) &&
            sync::detail::remove_no_symlinks(partial->temporary_path)) {
            ++removed_files;
        }
        writable->remove_partial_download(
            inspection.identity.drive_id, partial->item.remote_id
        );
    }
    for (const auto& path : orphaned) {
        if (sync::detail::remove_no_symlinks(path)) {
            ++removed_files;
        }
    }
    console.message(
        cli::MessageKind::success,
        "cleanup_completed",
        std::format(
            "Removed {} invalid partial records and {} partial files.",
            invalid.size(),
            removed_files
        )
    );
    return 0;
}

} // namespace onedrive::app::detail
