#include "sync/upload/orchestration.hpp"
#include "sync/upload/remote_delete.hpp"
#include "sync/upload/planning.hpp"
#include "onedrive/cli/console.hpp"
#include "util/typestate.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace onedrive::sync::detail {

using util::StateTransaction;
using util::TransactionState;
using util::TransactionStateFor;
using util::transition_transaction;

bool local_path_is_missing(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (!error) {
        return status.type() == std::filesystem::file_type::not_found;
    }
    if (error == std::errc::no_such_file_or_directory) {
        return true;
    }
    throw std::runtime_error(
        "cannot inspect local deletion candidate '" + path.string() +
        "': " + error.message()
    );
}

DeletionPlan discover_deletions(
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list,
    const std::unordered_set<std::string>& skipped_remote_ids
) {
    std::unordered_set<std::string> blocked_ids;
    std::unordered_set<std::string> blocked_paths;
    for (const auto& item : items.blocked_items(drive_id)) {
        blocked_ids.insert(item.remote_id);
        blocked_paths.insert(item.remote_path);
    }
    const auto tracked_items = items.drive_items(drive_id);
    std::vector<std::string> skipped_directory_paths;
    for (const auto& item : tracked_items) {
        if (item.directory && skipped_remote_ids.contains(item.remote_id)) {
            skipped_directory_paths.push_back(item.remote_path);
        }
    }
    std::vector<storage::PendingDelete> candidates;
    for (const auto& item : tracked_items) {
        if (item.remote_path.empty() ||
            skipped_remote_ids.contains(item.remote_id) ||
            std::ranges::any_of(
                skipped_directory_paths,
                [&item](const std::string& path) {
                    return remote_path_is_descendant(item.remote_path, path);
                }
            ) ||
            blocked_ids.contains(item.remote_id) ||
            blocked_paths.contains(item.remote_path) ||
            (sync_list != nullptr &&
             (sync_list->excludes(
                  item.remote_path,
                  item.directory ?
                      SyncItemKind::directory :
                      SyncItemKind::file,
                  !item.directory && item.size >= 0 ?
                      std::optional{static_cast<std::uint64_t>(item.size)} :
                      std::nullopt
              ) ||
              !sync_list->includes(
                  item.remote_path,
                  item.directory ?
                      SyncItemKind::directory :
                      SyncItemKind::file,
                  !item.directory && item.size >= 0 ?
                      std::optional{static_cast<std::uint64_t>(item.size)} :
                      std::nullopt
              ))) ||
            !local_path_is_missing(item.local_path)) {
            continue;
        }

        candidates.push_back({
            .drive_id = drive_id,
            .remote_id = item.remote_id,
            .expected_etag = item.etag,
            .remote_path = item.remote_path,
            .local_path = item.local_path,
            .directory = item.directory,
        });
    }
    std::ranges::sort(
        candidates,
        [](const auto& left, const auto& right) {
            const auto left_depth =
                std::ranges::distance(left.local_path);
            const auto right_depth =
                std::ranges::distance(right.local_path);
            return left_depth != right_depth ?
                left_depth < right_depth :
                left.remote_path < right.remote_path;
        }
    );
    std::vector<storage::PendingDelete> deletions;
    for (auto& candidate : candidates) {
        const bool covered = std::ranges::any_of(
            deletions,
            [&](const auto& parent) {
                return parent.directory &&
                       remote_path_is_descendant(
                           candidate.remote_path,
                           parent.remote_path
                       );
            }
        );
        if (!covered) {
            deletions.push_back(std::move(candidate));
        }
    }
    return {
        .operations = std::move(deletions),
        .affected_items = candidates.size(),
    };
}

DeletionPlan deletion_plan_for(
    std::vector<storage::PendingDelete> operations,
    const std::vector<storage::ItemState>& tracked_items
) {
    std::unordered_set<std::string> affected_ids;
    for (const auto& deletion : operations) {
        affected_ids.insert(deletion.remote_id);
        for (const auto& item : tracked_items) {
            if (item.remote_id == deletion.remote_id ||
                (deletion.directory &&
                 remote_path_is_descendant(
                     item.remote_path, deletion.remote_path
                 ))) {
                affected_ids.insert(item.remote_id);
            }
        }
    }
    return {
        .operations = std::move(operations),
        .affected_items = affected_ids.size(),
    };
}

bool enforce_remote_deletion_limit(
    const DeletionPlan& plan,
    RemoteDeletionGuard guard,
    const cli::Console& console,
    ExecutionMode execution_mode
) {
    if (plan.affected_items <= guard.maximum_affected_items) {
        return false;
    }
    const auto message = std::format(
        "Remote deletion plan affects {} tracked items, exceeding the "
        "configured limit of {}.",
        plan.affected_items,
        guard.maximum_affected_items
    );
    console.section(
        "large_delete_guard",
        "Large remote deletion safeguard:",
        {
            {
                .label = "Graph delete operations:",
                .key = "delete_operations",
                .value = std::to_string(plan.operations.size()),
            },
            {
                .label = "affected tracked items:",
                .key = "affected_items",
                .value = std::to_string(plan.affected_items),
            },
            {
                .label = "configured limit:",
                .key = "maximum_remote_deletions",
                .value = std::to_string(guard.maximum_affected_items),
            },
            {
                .label = "override:",
                .key = "forced",
                .value = guard.force ? "true" : "false",
            },
        }
    );
    if (guard.force) {
        spdlog::warn("{} Explicit override accepted.", message);
        console.message(
            cli::MessageKind::warning,
            "large_delete_forced",
            message + " Proceeding because --force-large-delete was provided."
        );
        return false;
    }
    if (execution_mode == ExecutionMode::preview) {
        console.message(
            cli::MessageKind::warning,
            "large_delete_detected",
            message + " A normal sync would be blocked."
        );
        return true;
    }
    spdlog::error("{}", message);
    console.message(
        cli::MessageKind::error,
        "large_delete_blocked",
        message + " Review the local filesystem and rerun sync with "
                  "--force-large-delete only if the deletions are intentional."
    );
    throw std::runtime_error{
        message + " Remote deletion was blocked by the large-delete safeguard."
    };
}

struct RemoteDeleteTransactionFamily;
using RemoteDeleteTransactionState =
    TransactionState<RemoteDeleteTransactionFamily>;
struct RemoteDeletePreparedState final : RemoteDeleteTransactionState {};
struct RemoteDeleteJournaledState final : RemoteDeleteTransactionState {};
struct RemoteDeleteGraphDeletedState final : RemoteDeleteTransactionState {};
struct RemoteDeleteLocalCommittedState final : RemoteDeleteTransactionState {};
struct RemoteDeleteTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, RemoteDeletePreparedState> &&
                std::same_as<Next, RemoteDeleteJournaledState>) ||
               (std::same_as<Current, RemoteDeleteJournaledState> &&
                std::same_as<Next, RemoteDeleteGraphDeletedState>) ||
               (std::same_as<Current, RemoteDeleteGraphDeletedState> &&
                std::same_as<Next, RemoteDeleteLocalCommittedState>);
    }
};

struct PendingRemoteDeletePayload {
    storage::PendingDelete deletion;
};

struct LocalCommittedRemoteDeletePayload {};

using PreparedRemoteDelete = StateTransaction<
    RemoteDeletePreparedState,
    RemoteDeleteTransactionFamily,
    PendingRemoteDeletePayload>;
using JournaledRemoteDelete = StateTransaction<
    RemoteDeleteJournaledState,
    RemoteDeleteTransactionFamily,
    PendingRemoteDeletePayload>;
using GraphDeletedRemoteDelete = StateTransaction<
    RemoteDeleteGraphDeletedState,
    RemoteDeleteTransactionFamily,
    PendingRemoteDeletePayload>;
using LocalCommittedRemoteDelete = StateTransaction<
    RemoteDeleteLocalCommittedState,
    RemoteDeleteTransactionFamily,
    LocalCommittedRemoteDeletePayload>;

JournaledRemoteDelete
journal_remote_delete(PreparedRemoteDelete transaction) noexcept {
    return transition_transaction<RemoteDeleteJournaledState>(
        std::move(transaction)
    );
}

GraphDeletedRemoteDelete
mark_remote_delete_graph_deleted(JournaledRemoteDelete transaction) noexcept {
    return transition_transaction<RemoteDeleteGraphDeletedState>(
        std::move(transaction)
    );
}

LocalCommittedRemoteDelete mark_remote_delete_local_committed(
    GraphDeletedRemoteDelete transaction
) noexcept {
    return transition_transaction<RemoteDeleteLocalCommittedState>(
        std::move(transaction),
        [](PendingRemoteDeletePayload&&) noexcept {
            return LocalCommittedRemoteDeletePayload{};
        }
    );
}

template <typename Transaction>
concept JournalableRemoteDelete = requires(Transaction transaction) {
    journal_remote_delete(std::move(transaction));
};

template <typename Transaction>
concept GraphDeletableRemoteDelete = requires(Transaction transaction) {
    mark_remote_delete_graph_deleted(std::move(transaction));
};

template <typename Transaction>
concept LocallyCommittableRemoteDelete = requires(Transaction transaction) {
    mark_remote_delete_local_committed(std::move(transaction));
};

template <typename Transaction>
concept HasPendingRemoteDelete =
    requires(Transaction transaction) { transaction.deletion.remote_id; };

static_assert(JournalableRemoteDelete<PreparedRemoteDelete>);
static_assert(!JournalableRemoteDelete<JournaledRemoteDelete>);
static_assert(GraphDeletableRemoteDelete<JournaledRemoteDelete>);
static_assert(!GraphDeletableRemoteDelete<PreparedRemoteDelete>);
static_assert(LocallyCommittableRemoteDelete<GraphDeletedRemoteDelete>);
static_assert(!LocallyCommittableRemoteDelete<JournaledRemoteDelete>);
static_assert(HasPendingRemoteDelete<PreparedRemoteDelete>);
static_assert(HasPendingRemoteDelete<JournaledRemoteDelete>);
static_assert(HasPendingRemoteDelete<GraphDeletedRemoteDelete>);
static_assert(!HasPendingRemoteDelete<LocalCommittedRemoteDelete>);

LocalCommittedRemoteDelete execute_pending_delete(
    JournaledRemoteDelete transaction,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    const auto& deletion = transaction.deletion;
    if (!local_path_is_missing(deletion.local_path)) {
        items.remove_pending_delete(
            deletion.drive_id,
            deletion.remote_id
        );
        throw LocalModificationConflictError(
            "local item reappeared during remote deletion: " +
            deletion.local_path.string()
        );
    }
    try {
        graph.delete_item(deletion.remote_id, deletion.expected_etag);
    } catch (const graph::UploadConflictError&) {
        items.remove_pending_delete(
            deletion.drive_id,
            deletion.remote_id
        );
        throw LocalModificationConflictError(
            "remote item changed after local deletion: " +
            deletion.remote_path
        );
    }
    auto graph_deleted =
        mark_remote_delete_graph_deleted(std::move(transaction));
    items.commit_delete(graph_deleted.deletion);
    return mark_remote_delete_local_committed(std::move(graph_deleted));
}

void execute_new_remote_delete(
    storage::PendingDelete deletion,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    auto prepared = PreparedRemoteDelete{
        PendingRemoteDeletePayload{std::move(deletion)},
    };
    items.save_pending_delete(prepared.deletion);
    auto journaled = journal_remote_delete(std::move(prepared));
    static_cast<void>(
        execute_pending_delete(std::move(journaled), graph, items)
    );
}

void recover_pending_deletes(
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console,
    RemoteDeletionGuard deletion_guard
) {
    std::vector<storage::PendingDelete> active;
    for (const auto& deletion : items.pending_deletes(drive_id)) {
        if (local_path_is_missing(deletion.local_path)) {
            active.push_back(deletion);
            continue;
        }
        auto journaled = JournaledRemoteDelete{
            PendingRemoteDeletePayload{deletion},
        };
        static_cast<void>(
            execute_pending_delete(std::move(journaled), graph, items)
        );
        console.message(
            cli::MessageKind::information,
            "pending_delete_cancelled",
            "Cancelled pending remote deletion because the local item "
            "reappeared: '" +
                deletion.remote_path + "'."
        );
    }
    const auto plan =
        deletion_plan_for(std::move(active), items.drive_items(drive_id));
    static_cast<void>(
        enforce_remote_deletion_limit(
            plan,
            deletion_guard,
            console,
            ExecutionMode::apply
        )
    );
    for (const auto& deletion : plan.operations) {
        auto journaled = JournaledRemoteDelete{
            PendingRemoteDeletePayload{deletion},
        };
        static_cast<void>(
            execute_pending_delete(std::move(journaled), graph, items)
        );
        console.message(
            cli::MessageKind::information,
            "pending_delete_recovered",
            "Recovered remote deletion '" + deletion.remote_path + "'."
        );
    }
}

}  // namespace onedrive::sync::detail
