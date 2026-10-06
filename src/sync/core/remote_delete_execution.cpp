#include "sync/core/remote_delete_execution.hpp"

#include "onedrive/cli/console.hpp"
#include "sync/core/item_operation_coordinator.hpp"
#include "sync/core/operation_reporting.hpp"
#include "sync/core/plan.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onedrive::sync::engine_detail {

std::size_t execute_removals(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const cli::Console& console
) {
    struct Removal {
        graph::RemoteItem item;
        std::optional<storage::ItemState> previous;
    };
    std::vector<Removal> removals;
    removals.reserve(plan.removal_count());
    for (std::size_t index = 0; index < plan.removal_count(); ++index) {
        const auto& item = plan.removal(index);
        removals.push_back({
            .item = item,
            .previous = items.find(drive_id, item.id),
        });
    }
    std::ranges::sort(
        removals,
        [](const Removal& left, const Removal& right) {
            if (!left.previous || !right.previous) {
                return left.previous.has_value();
            }
            const auto left_depth = std::ranges::count(
                left.previous->remote_path,
                '/'
            );
            const auto right_depth = std::ranges::count(
                right.previous->remote_path,
                '/'
            );
            if (left_depth != right_depth) {
                return left_depth > right_depth;
            }
            return !left.previous->directory &&
                   right.previous->directory;
        }
    );
    std::unordered_map<std::string, storage::PendingMove> pending_moves;
    for (auto pending : items.pending_moves(drive_id)) {
        pending_moves.emplace(pending.remote_id, std::move(pending));
    }

    std::size_t removed = 0;
    for (const auto& removal : removals) {
        if (!removal.previous) {
            plan.complete_removal(removal.item.id);
            continue;
        }
        const auto& previous = *removal.previous;
        auto block = [&](std::string code, std::string message) {
            plan.block_removal(
                previous,
                std::move(code),
                std::move(message)
            );
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        };
        try {
            auto operation = operations.acquire(drive_id, previous.remote_id);
            auto local_path = previous.local_path;
            if (const auto pending =
                    pending_moves.find(previous.remote_id);
                pending != pending_moves.end()) {
                std::vector<std::filesystem::path> candidates{
                    pending->second.destination_path,
                };
                if (!pending->second.staging_path.empty()) {
                    candidates.push_back(pending->second.staging_path);
                }
                candidates.push_back(pending->second.source_path);
                bool candidate_exists = false;
                bool identity_found = false;
                for (const auto& candidate : candidates) {
                    std::error_code candidate_error;
                    const auto candidate_status =
                        std::filesystem::symlink_status(
                            candidate,
                            candidate_error
                        );
                    if (candidate_error ==
                        std::errc::no_such_file_or_directory) {
                        continue;
                    }
                    if (candidate_error) {
                        throw std::runtime_error(
                            "cannot inspect pending move path before remote "
                            "deletion: " + candidate_error.message()
                        );
                    }
                    if (!std::filesystem::exists(candidate_status)) {
                        continue;
                    }
                    candidate_exists = true;
                    if (std::filesystem::is_symlink(candidate_status) ||
                        (previous.directory &&
                         !std::filesystem::is_directory(candidate_status)) ||
                        (!previous.directory &&
                         !std::filesystem::is_regular_file(
                             candidate_status
                         ))) {
                        continue;
                    }
                    const auto identity = safe_root.identity(
                        candidate,
                        previous.directory
                    );
                    if (identity.device ==
                            pending->second.source_device &&
                        identity.inode ==
                            pending->second.source_inode) {
                        local_path = candidate;
                        identity_found = true;
                        break;
                    }
                }
                if (!identity_found && candidate_exists) {
                    block(
                        "pending_move_conflict",
                        "pending move object identity changed before remote "
                        "deletion: " + previous.remote_path
                    );
                    continue;
                }
                if (!identity_found) {
                    plan.complete_removal(previous.remote_id);
                    continue;
                }
            }
            auto destination_operation =
                operations.acquire_destination(local_path);
            static_cast<void>(
                safe_root.relative_path(local_path)
            );
            std::error_code error;
            const auto status =
                std::filesystem::symlink_status(local_path, error);
            if (error == std::errc::no_such_file_or_directory ||
                !std::filesystem::exists(status)) {
                plan.complete_removal(previous.remote_id);
                continue;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot inspect local deletion target '" +
                    local_path.string() + "': " + error.message()
                );
            }
            if (std::filesystem::is_symlink(status) ||
                (previous.directory &&
                 !std::filesystem::is_directory(status)) ||
                (!previous.directory &&
                 !std::filesystem::is_regular_file(status))) {
                block(
                    "local_path_conflict",
                    "remote deletion target has an unexpected local type: " +
                        local_path.string()
                );
                continue;
            }
            if (!previous.directory &&
                !detail::local_snapshot_matches(
                    previous,
                    local_path
                )) {
                block(
                    "local_modification",
                    "local file changed after the last synchronized snapshot: " +
                        local_path.string()
                );
                continue;
            }
            try {
                if (safe_root.remove(
                        local_path,
                        previous.directory
                    )) {
                    ++removed;
                    console.message(
                        cli::MessageKind::information,
                        "local_item_removed",
                        "Removed remotely deleted local item '" +
                            local_path.string() + "'."
                    );
                }
                plan.complete_removal(previous.remote_id);
            } catch (const detail::SafePathConflictError& exception) {
                block("local_path_conflict", exception.what());
            }
        } catch (const detail::SafePathConflictError& exception) {
            block("local_path_conflict", exception.what());
        }
    }
    return removed;
}

}  // namespace onedrive::sync::engine_detail
