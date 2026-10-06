#include "sync/core/plan_report.hpp"

#include "onedrive/cli/console.hpp"
#include "sync/core/reporting.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <string>

namespace onedrive::sync::engine_detail {

void report_plan(
    const detail::SyncPlan& plan,
    const std::string& drive_id,
    const cli::Console& console,
    SyncCapabilities capabilities
) {
    const auto directory_count =
        capabilities.plans_downloads() ? plan.directory_count() : 0;
    const auto download_count =
        capabilities.plans_downloads() ? plan.download_count() : 0;
    const auto download_bytes =
        capabilities.plans_downloads() ? plan.download_bytes() : 0;
    const auto move_count =
        capabilities.plans_downloads() ? plan.move_count() : 0;
    const auto removal_count =
        capabilities.plans_local_deletions() ? plan.removal_count() : 0;
    const auto upsert_count = directory_count + download_count;
    console.message(
        cli::MessageKind::information,
        "remote_delta",
        std::format(
            "Remote delta contains {} changes ({} upserts, {} removals, {} "
            "moves, {} blocked).",
            plan.change_count(),
            upsert_count,
            removal_count,
            move_count,
            plan.blocked_count()
        )
    );
    spdlog::info(
        "Remote delta prepared for drive '{}': {} upserts, {} removals, {} "
        "moves, {} blocked",
        drive_id,
        upsert_count,
        removal_count,
        move_count,
        plan.blocked_count()
    );
    spdlog::info(
        "Synchronization plan for drive '{}': {} directories, {} downloads, "
        "{} bytes, {} deferred local removals",
        drive_id,
        directory_count,
        download_count,
        download_bytes,
        removal_count
    );
    if (removal_count != 0) {
        spdlog::info(
            "{} remote deletions are eligible for safe local execution",
            removal_count
        );
    }
    console.section(
        "synchronization_plan",
        "Synchronization plan:",
        {
            {
                .label = "create directories:",
                .key = "create_directories",
                .value = std::to_string(directory_count),
            },
            {
                .label = "download files:",
                .key = "download_files",
                .value = std::to_string(download_count),
            },
            {
                .label = "download bytes:",
                .key = "download_bytes",
                .value = std::to_string(download_bytes),
            },
            {
                .label = "blocked items:",
                .key = "blocked_items",
                .value = std::to_string(plan.blocked_count()),
            },
            {
                .label = "local moves:",
                .key = "local_moves",
                .value = std::to_string(move_count),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = std::to_string(removal_count),
            },
        }
    );
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        engine_detail::report_blocked(plan.blocked(index), console);
    }
}

}  // namespace onedrive::sync::engine_detail
