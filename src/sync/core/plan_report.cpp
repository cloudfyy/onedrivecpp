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
    const cli::Console& console
) {
    const auto upsert_count =
        plan.directory_count() + plan.download_count();
    console.message(
        cli::MessageKind::information,
        "remote_delta",
        std::format(
            "Remote delta contains {} changes ({} upserts, {} removals, {} "
            "moves, {} blocked).",
            plan.change_count(),
            upsert_count,
            plan.removal_count(),
            plan.move_count(),
            plan.blocked_count()
        )
    );
    spdlog::info(
        "Remote delta prepared for drive '{}': {} upserts, {} removals, {} "
        "moves, {} blocked",
        drive_id,
        upsert_count,
        plan.removal_count(),
        plan.move_count(),
        plan.blocked_count()
    );
    spdlog::info(
        "Synchronization plan for drive '{}': {} directories, {} downloads, "
        "{} bytes, {} deferred local removals",
        drive_id,
        plan.directory_count(),
        plan.download_count(),
        plan.download_bytes(),
        plan.removal_count()
    );
    if (plan.removal_count() != 0) {
        spdlog::info(
            "{} remote deletions are eligible for safe local execution",
            plan.removal_count()
        );
    }
    console.section(
        "synchronization_plan",
        "Synchronization plan:",
        {
            {
                .label = "create directories:",
                .key = "create_directories",
                .value = std::to_string(plan.directory_count()),
            },
            {
                .label = "download files:",
                .key = "download_files",
                .value = std::to_string(plan.download_count()),
            },
            {
                .label = "download bytes:",
                .key = "download_bytes",
                .value = std::to_string(plan.download_bytes()),
            },
            {
                .label = "blocked items:",
                .key = "blocked_items",
                .value = std::to_string(plan.blocked_count()),
            },
            {
                .label = "local moves:",
                .key = "local_moves",
                .value = std::to_string(plan.move_count()),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = std::to_string(plan.removal_count()),
            },
        }
    );
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        engine_detail::report_blocked(plan.blocked(index), console);
    }
}

}  // namespace onedrive::sync::engine_detail
