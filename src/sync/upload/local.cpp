#include "sync/upload/local.hpp"

#include "util/unique_file_descriptor.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/core/typestate.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

struct UploadCandidate {
    std::filesystem::path path;
    std::string remote_path;
    std::optional<storage::ItemState> previous;
    bool directory{false};
};

struct DeletionPlan {
    std::vector<storage::PendingDelete> operations;
    std::size_t affected_items{0};
};

class RemoteUploadConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LocalUploadResourceError final : public std::runtime_error {
public:
    LocalUploadResourceError(std::string reason_code, std::string message)
        : std::runtime_error{std::move(message)},
          reason_code_{std::move(reason_code)} {}

    [[nodiscard]] const std::string& reason_code() const noexcept {
        return reason_code_;
    }

private:
    std::string reason_code_;
};

std::string local_resource_code(
    const std::error_code& error,
    std::string_view fallback
) {
    if (error == std::errc::permission_denied ||
        error == std::errc::read_only_file_system) {
        return "local_permission";
    }
    if (error == std::errc::no_space_on_device ||
        error == std::errc::file_too_large) {
        return "local_storage";
    }
    return std::string{fallback};
}

struct LocalMoveDiscovery {
    std::vector<storage::PendingRemoteMove> moves;
    std::unordered_set<std::string> moved_remote_ids;
    std::unordered_set<std::string> moved_local_paths;
};

bool reserved_local_name(const std::filesystem::path& path);

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
    const std::unordered_set<std::string>& skipped_remote_ids = {}
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
             (sync_list->excludes(item.remote_path, item.directory) ||
              !sync_list->includes(item.remote_path, item.directory))) ||
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
    RemoteDeletionPolicy policy,
    const cli::Console& console,
    bool dry_run
) {
    if (plan.affected_items <= policy.maximum_affected_items) {
        return false;
    }
    const auto message = std::format(
        "Remote deletion plan affects {} tracked items, exceeding the "
        "configured limit of {}.",
        plan.affected_items,
        policy.maximum_affected_items
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
                .value = std::to_string(policy.maximum_affected_items),
            },
            {
                .label = "override:",
                .key = "forced",
                .value = policy.force ? "true" : "false",
            },
        }
    );
    if (policy.force) {
        spdlog::warn("{} Explicit override accepted.", message);
        console.message(
            cli::MessageKind::warning,
            "large_delete_forced",
            message + " Proceeding because --force-large-delete was provided."
        );
        return false;
    }
    if (dry_run) {
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

LocalMoveDiscovery discover_local_moves(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list
) {
    struct CurrentItem {
        std::filesystem::path path;
        std::string remote_path;
        bool directory{false};
    };
    auto identity_key = [](std::uint64_t device, std::uint64_t inode) {
        return std::to_string(device) + ":" + std::to_string(inode);
    };
    std::unordered_map<std::string, CurrentItem> current;
    std::unordered_set<std::string> ambiguous;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator{
        sync_root.path(),
        std::filesystem::directory_options::none,
        error
    };
    if (error) {
        throw std::runtime_error(
            "cannot scan synchronization directory for local moves: " +
            error.message()
        );
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto path = iterator->path();
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect local move candidate '" + path.string() +
                "': " + error.message()
            );
        }
        const bool directory = std::filesystem::is_directory(status);
        const bool regular = std::filesystem::is_regular_file(status);
        if (std::filesystem::is_symlink(status) ||
            (!directory && !regular) || reserved_local_name(path)) {
            if (directory) {
                iterator.disable_recursion_pending();
            }
        } else {
            const auto relative =
                path.lexically_relative(sync_root.path());
            const auto remote_path = relative.generic_string();
            const auto identity = sync_root.identity(path, directory);
            const auto key = identity_key(
                identity.device,
                identity.inode
            );
            if (!current.emplace(
                    key,
                    CurrentItem{path, remote_path, directory}
                ).second) {
                ambiguous.insert(key);
            }
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(
                "cannot continue local move scan: " + error.message()
            );
        }
    }

    const auto tracked = items.drive_items(drive_id);
    std::unordered_set<std::string> tracked_paths;
    for (const auto& item : tracked) {
        tracked_paths.insert(item.remote_path);
    }
    std::unordered_set<std::string> moved_directory_paths;
    for (const auto& item : tracked) {
        if (!item.directory || item.local_device == 0 ||
            item.local_inode == 0 ||
            !local_path_is_missing(item.local_path)) {
            continue;
        }
        const auto found = current.find(
            identity_key(item.local_device, item.local_inode)
        );
        if (found != current.end() && found->second.directory) {
            moved_directory_paths.insert(found->second.remote_path);
        }
    }
    LocalMoveDiscovery discovery;
    for (const auto& item : tracked) {
        if (item.local_device == 0 || item.local_inode == 0 ||
            !local_path_is_missing(item.local_path)) {
            continue;
        }
        const auto key =
            identity_key(item.local_device, item.local_inode);
        const auto found = current.find(key);
        if (found == current.end() || ambiguous.contains(key) ||
            found->second.directory != item.directory ||
            found->second.path == item.local_path) {
            continue;
        }
        discovery.moved_remote_ids.insert(item.remote_id);
        discovery.moved_local_paths.insert(
            found->second.path.lexically_normal().string()
        );
        if (sync_list != nullptr &&
            (sync_list->excludes(
                 found->second.remote_path,
                 item.directory
             ) ||
             !sync_list->includes(
                 found->second.remote_path,
                 item.directory
             ))) {
            continue;
        }
        const auto parent =
            std::filesystem::path{found->second.remote_path}.
                parent_path().generic_string();
        if (!parent.empty() && !tracked_paths.contains(parent) &&
            !moved_directory_paths.contains(parent)) {
            const auto local_parent = sync_root.path() / parent;
            std::error_code parent_error;
            if (!std::filesystem::is_directory(
                    local_parent,
                    parent_error
                ) ||
                parent_error) {
                throw LocalModificationConflictError(
                    "local move destination parent is unavailable: " +
                    found->second.remote_path
                );
            }
        }
        discovery.moves.push_back({
            .drive_id = drive_id,
            .remote_id = item.remote_id,
            .expected_etag = item.etag,
            .source_remote_path = item.remote_path,
            .destination_remote_path = found->second.remote_path,
            .source_local_path = item.local_path,
            .destination_local_path = found->second.path,
            .local_device = item.local_device,
            .local_inode = item.local_inode,
            .directory = item.directory,
        });
    }
    std::ranges::sort(
        discovery.moves,
        [](const auto& left, const auto& right) {
            return std::ranges::distance(left.source_local_path) <
                   std::ranges::distance(right.source_local_path);
        }
    );
    std::vector<storage::PendingRemoteMove> roots;
    for (auto& move : discovery.moves) {
        const bool covered = std::ranges::any_of(
            roots,
            [&](const auto& parent) {
                if (!parent.directory) {
                    return false;
                }
                const auto source_relative =
                    move.source_local_path.lexically_relative(
                        parent.source_local_path
                    );
                const auto destination_relative =
                    move.destination_local_path.lexically_relative(
                        parent.destination_local_path
                    );
                return !source_relative.empty() &&
                       !source_relative.native().starts_with("..") &&
                       source_relative == destination_relative;
            }
        );
        if (!covered) {
            roots.push_back(std::move(move));
        }
    }
    discovery.moves = std::move(roots);
    return discovery;
}

std::vector<UploadCandidate> discover_move_parent_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const std::vector<storage::PendingRemoteMove>& moves
) {
    std::unordered_set<std::string> tracked_paths;
    for (const auto& item : items.drive_items(drive_id)) {
        tracked_paths.insert(item.remote_path);
    }
    std::unordered_set<std::string> moved_directory_paths;
    for (const auto& move : moves) {
        if (move.directory) {
            moved_directory_paths.insert(move.destination_remote_path);
        }
    }
    std::unordered_map<std::string, UploadCandidate> parents;
    for (const auto& move : moves) {
        auto parent =
            std::filesystem::path{move.destination_remote_path}.
                parent_path();
        while (!parent.empty()) {
            const auto remote_path = parent.generic_string();
            if (tracked_paths.contains(remote_path)) {
                break;
            }
            if (!moved_directory_paths.contains(remote_path)) {
                parents.try_emplace(
                    remote_path,
                    UploadCandidate{
                        .path = sync_root.path() / parent,
                        .remote_path = remote_path,
                        .previous = std::nullopt,
                        .directory = true,
                    }
                );
            }
            parent = parent.parent_path();
        }
    }
    std::vector<UploadCandidate> result;
    result.reserve(parents.size());
    for (auto& [remote_path, candidate] : parents) {
        static_cast<void>(remote_path);
        result.push_back(std::move(candidate));
    }
    std::ranges::sort(
        result,
        [](const auto& left, const auto& right) {
            const auto left_depth =
                std::ranges::distance(left.path);
            const auto right_depth =
                std::ranges::distance(right.path);
            return left_depth != right_depth ?
                left_depth < right_depth :
                left.remote_path < right.remote_path;
        }
    );
    return result;
}

struct RemoteDeleteTransactionFamily;
using RemoteDeleteTransactionState =
    TransactionState<RemoteDeleteTransactionFamily>;
struct RemoteDeletePreparedState final : RemoteDeleteTransactionState {};
struct RemoteDeleteJournaledState final : RemoteDeleteTransactionState {};
struct RemoteDeleteGraphDeletedState final : RemoteDeleteTransactionState {};
struct RemoteDeleteLocalCommittedState final : RemoteDeleteTransactionState {};

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

bool local_move_identity_matches(
    const SafeSyncRoot& sync_root,
    const storage::PendingRemoteMove& move
) {
    try {
        const auto identity = sync_root.identity(
            move.destination_local_path,
            move.directory
        );
        return identity.device == move.local_device &&
               identity.inode == move.local_inode;
    } catch (const std::exception&) {
        return false;
    }
}

struct RemoteMoveTransactionFamily;
using RemoteMoveTransactionState =
    TransactionState<RemoteMoveTransactionFamily>;
struct RemoteMovePreparedState final : RemoteMoveTransactionState {};
struct RemoteMoveJournaledState final : RemoteMoveTransactionState {};
struct RemoteMoveGraphCommittedState final : RemoteMoveTransactionState {};
struct RemoteMoveLocalCommittedState final : RemoteMoveTransactionState {};

template <typename State>
concept RemoteMoveState =
    TransactionStateFor<State, RemoteMoveTransactionFamily>;

struct PendingRemoteMovePayload {
    storage::PendingRemoteMove move;
    storage::ItemState previous;
};

struct GraphCommittedRemoteMovePayload {
    storage::PendingRemoteMove move;
    storage::ItemState previous;
    graph::RemoteItem remote;
};

struct LocalCommittedRemoteMovePayload {
    storage::PendingRemoteMove move;
};

using PreparedRemoteMove = StateTransaction<
    RemoteMovePreparedState,
    RemoteMoveTransactionFamily,
    PendingRemoteMovePayload>;
using JournaledRemoteMove = StateTransaction<
    RemoteMoveJournaledState,
    RemoteMoveTransactionFamily,
    PendingRemoteMovePayload>;
using GraphCommittedRemoteMove = StateTransaction<
    RemoteMoveGraphCommittedState,
    RemoteMoveTransactionFamily,
    GraphCommittedRemoteMovePayload>;
using LocalCommittedRemoteMove = StateTransaction<
    RemoteMoveLocalCommittedState,
    RemoteMoveTransactionFamily,
    LocalCommittedRemoteMovePayload>;

static_assert(std::is_nothrow_move_constructible_v<PreparedRemoteMove>);
static_assert(std::is_nothrow_move_constructible_v<JournaledRemoteMove>);
static_assert(std::is_nothrow_move_constructible_v<GraphCommittedRemoteMove>);
static_assert(std::is_nothrow_move_constructible_v<LocalCommittedRemoteMove>);

JournaledRemoteMove
journal_remote_move(PreparedRemoteMove transaction) noexcept {
    return transition_transaction<RemoteMoveJournaledState>(
        std::move(transaction)
    );
}

GraphCommittedRemoteMove mark_graph_remote_move_committed(
    JournaledRemoteMove transaction, graph::RemoteItem remote
) noexcept {
    return transition_transaction<RemoteMoveGraphCommittedState>(
        std::move(transaction),
        [remote = std::move(remote)](
            PendingRemoteMovePayload&& payload
        ) mutable noexcept {
            return GraphCommittedRemoteMovePayload{
                .move = std::move(payload.move),
                .previous = std::move(payload.previous),
                .remote = std::move(remote),
            };
        }
    );
}

LocalCommittedRemoteMove mark_local_remote_move_committed(
    GraphCommittedRemoteMove transaction
) noexcept {
    return transition_transaction<RemoteMoveLocalCommittedState>(
        std::move(transaction),
        [](GraphCommittedRemoteMovePayload&& payload) noexcept {
            return LocalCommittedRemoteMovePayload{
                .move = std::move(payload.move),
            };
        }
    );
}

template <typename Transaction>
concept JournalableRemoteMove = requires(Transaction transaction) {
    journal_remote_move(std::move(transaction));
};

template <typename Transaction>
concept GraphCommittableRemoteMove = requires(
    Transaction transaction, graph::RemoteItem remote
) {
    mark_graph_remote_move_committed(std::move(transaction), std::move(remote));
};

template <typename Transaction>
concept LocalCommittableRemoteMove = requires(Transaction transaction) {
    mark_local_remote_move_committed(std::move(transaction));
};

template <typename Transaction>
concept HasRemoteMoveResult =
    requires(Transaction transaction) { transaction.remote.id; };

static_assert(JournalableRemoteMove<PreparedRemoteMove>);
static_assert(!JournalableRemoteMove<JournaledRemoteMove>);
static_assert(!JournalableRemoteMove<GraphCommittedRemoteMove>);
static_assert(!JournalableRemoteMove<LocalCommittedRemoteMove>);
static_assert(GraphCommittableRemoteMove<JournaledRemoteMove>);
static_assert(!GraphCommittableRemoteMove<PreparedRemoteMove>);
static_assert(!GraphCommittableRemoteMove<GraphCommittedRemoteMove>);
static_assert(LocalCommittableRemoteMove<GraphCommittedRemoteMove>);
static_assert(!LocalCommittableRemoteMove<JournaledRemoteMove>);
static_assert(!HasRemoteMoveResult<PreparedRemoteMove>);
static_assert(!HasRemoteMoveResult<JournaledRemoteMove>);
static_assert(HasRemoteMoveResult<GraphCommittedRemoteMove>);
static_assert(!HasRemoteMoveResult<LocalCommittedRemoteMove>);

LocalCommittedRemoteMove execute_pending_remote_move(
    const SafeSyncRoot& sync_root,
    JournaledRemoteMove transaction,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    const auto& move = transaction.move;
    if (!local_move_identity_matches(sync_root, move)) {
        items.remove_pending_remote_move(move.drive_id, move.remote_id);
        throw LocalModificationConflictError(
            "local move destination identity changed: " +
            move.destination_local_path.string()
        );
    }
    graph::RemoteItem remote;
    try {
        remote = graph.move_item(
            move.remote_id,
            move.expected_etag,
            move.destination_remote_path
        );
    } catch (const graph::UploadConflictError&) {
        const auto existing =
            graph.item_by_path(move.destination_remote_path);
        if (existing.id != move.remote_id ||
            existing.directory != move.directory) {
            items.remove_pending_remote_move(
                move.drive_id,
                move.remote_id
            );
            throw LocalModificationConflictError(
                "remote move conflicts with '" +
                move.destination_remote_path + "'"
            );
        }
        remote = existing;
    }
    auto graph_committed = mark_graph_remote_move_committed(
        std::move(transaction), std::move(remote)
    );
    const auto& committed_move = graph_committed.move;
    const auto& committed_remote = graph_committed.remote;
    if (committed_remote.id != committed_move.remote_id ||
        committed_remote.remote_path !=
            committed_move.destination_remote_path ||
        committed_remote.directory != committed_move.directory ||
        !local_move_identity_matches(sync_root, committed_move)) {
        throw LocalModificationConflictError(
            "remote move result does not match local identity for '" +
            committed_move.destination_remote_path + "'"
        );
    }
    auto state = graph_committed.previous;
    state.parent_id = committed_remote.parent_id;
    state.name = committed_remote.name;
    state.etag = committed_remote.etag;
    state.ctag = committed_remote.ctag;
    state.remote_path = committed_remote.remote_path;
    state.local_path = committed_move.destination_local_path;
    state.last_modified = committed_remote.last_modified;
    state.size = committed_remote.size;
    state.local_device = committed_move.local_device;
    state.local_inode = committed_move.local_inode;
    items.commit_remote_move(committed_move, std::move(state));
    return mark_local_remote_move_committed(std::move(graph_committed));
}

class UploadSnapshot final {
public:
    explicit UploadSnapshot(std::filesystem::path path)
        : path_{std::move(path)} {}

    ~UploadSnapshot() {
        if (!path_.empty()) {
            try {
                static_cast<void>(remove_no_symlinks(path_));
            } catch (const std::exception& error) {
                spdlog::warn(
                    "Cannot remove upload snapshot '{}': {}",
                    path_.string(),
                    error.what()
                );
            }
        }
    }

    UploadSnapshot(const UploadSnapshot&) = delete;
    UploadSnapshot& operator=(const UploadSnapshot&) = delete;
    UploadSnapshot(UploadSnapshot&& other) noexcept
        : path_{std::exchange(other.path_, {})} {}
    UploadSnapshot& operator=(UploadSnapshot&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path release() noexcept {
        return std::exchange(path_, {});
    }

private:
    std::filesystem::path path_;
};

struct UploadTransactionFamily;
using UploadTransactionState = TransactionState<UploadTransactionFamily>;
struct UploadPreparedState final : UploadTransactionState {};
struct UploadJournaledState final : UploadTransactionState {};
struct UploadRemoteCommittedState final : UploadTransactionState {};

template <typename State>
concept UploadState = TransactionStateFor<State, UploadTransactionFamily>;

struct PreparedUploadPayload {
    storage::PendingUpload* pending{};
    LocalFileBaseline baseline;
    UploadSnapshot snapshot;
};

struct JournaledUploadPayload {
    storage::PendingUpload* pending{};
    LocalFileBaseline baseline;
};

struct RemoteCommittedUploadPayload {
    storage::PendingUpload* pending{};
    LocalFileBaseline baseline;
    graph::RemoteItem remote;
};

using PreparedUpload = StateTransaction<
    UploadPreparedState,
    UploadTransactionFamily,
    PreparedUploadPayload>;
using JournaledUpload = StateTransaction<
    UploadJournaledState,
    UploadTransactionFamily,
    JournaledUploadPayload>;
using RemoteCommittedUpload = StateTransaction<
    UploadRemoteCommittedState,
    UploadTransactionFamily,
    RemoteCommittedUploadPayload>;

static_assert(std::is_nothrow_move_constructible_v<PreparedUpload>);
static_assert(std::is_nothrow_move_constructible_v<JournaledUpload>);
static_assert(std::is_nothrow_move_constructible_v<RemoteCommittedUpload>);
static_assert(!std::same_as<PreparedUpload, JournaledUpload>);
static_assert(!std::same_as<JournaledUpload, RemoteCommittedUpload>);

template <UploadState State, typename Payload>
storage::PendingUpload& pending_upload(
    StateTransaction<State, UploadTransactionFamily, Payload>& upload
) noexcept {
    return *upload.pending;
}

JournaledUpload journal_upload(PreparedUpload upload) noexcept {
    return transition_transaction<UploadJournaledState>(
        std::move(upload),
        [](PreparedUploadPayload&& payload) noexcept {
            static_cast<void>(payload.snapshot.release());
            return JournaledUploadPayload{
                .pending = payload.pending,
                .baseline = std::move(payload.baseline),
            };
        }
    );
}

RemoteCommittedUpload mark_remote_upload_committed(
    JournaledUpload upload, graph::RemoteItem remote
) noexcept {
    return transition_transaction<UploadRemoteCommittedState>(
        std::move(upload),
        [remote = std::move(remote)](
            JournaledUploadPayload&& payload
        ) mutable noexcept {
            return RemoteCommittedUploadPayload{
                .pending = payload.pending,
                .baseline = std::move(payload.baseline),
                .remote = std::move(remote),
            };
        }
    );
}

template <typename Transaction>
concept JournalableUpload = requires(Transaction transaction) {
    journal_upload(std::move(transaction));
};

template <typename Transaction>
concept RemoteCommittableUpload =
    requires(Transaction transaction, graph::RemoteItem remote) {
        mark_remote_upload_committed(std::move(transaction), std::move(remote));
    };

template <typename Transaction>
concept HasUploadSnapshot =
    requires(Transaction transaction) { transaction.snapshot.release(); };

template <typename Transaction>
concept HasRemoteUploadResult =
    requires(Transaction transaction) { transaction.remote.id; };

static_assert(JournalableUpload<PreparedUpload>);
static_assert(!JournalableUpload<JournaledUpload>);
static_assert(!JournalableUpload<RemoteCommittedUpload>);
static_assert(RemoteCommittableUpload<JournaledUpload>);
static_assert(!RemoteCommittableUpload<PreparedUpload>);
static_assert(!RemoteCommittableUpload<RemoteCommittedUpload>);
static_assert(HasUploadSnapshot<PreparedUpload>);
static_assert(!HasUploadSnapshot<JournaledUpload>);
static_assert(!HasUploadSnapshot<RemoteCommittedUpload>);
static_assert(!HasRemoteUploadResult<PreparedUpload>);
static_assert(!HasRemoteUploadResult<JournaledUpload>);
static_assert(HasRemoteUploadResult<RemoteCommittedUpload>);

graph::UploadCheckpoint
persist_upload_checkpoints(JournaledUpload& upload, storage::ItemStore& items) {
    return [&](const graph::UploadSession& state) {
        auto& pending = pending_upload(upload);
        pending.upload_url = state.upload_url;
        pending.upload_expiration = state.expiration;
        pending.completed_bytes = state.completed_bytes;
        items.save_pending_upload(pending);
    };
}

bool reserved_local_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.contains(".safeBackup-") ||
           name.contains(".onedrive-partial-") ||
           name.contains(".onedrive-upload-") ||
           name.contains(".onedrive-move-");
}

std::filesystem::path upload_snapshot_path(
    const std::filesystem::path& source,
    std::size_t attempt
) {
    return source.parent_path() /
           ("." + source.filename().string() + ".onedrive-upload-" +
            std::to_string(::getpid()) + "-" + std::to_string(attempt));
}

UploadSnapshot create_upload_snapshot(
    const std::filesystem::path& source,
    const LocalFileBaseline& baseline
) {
    onedrive::util::UniqueFileDescriptor input;
    try {
        input.reset(
            onedrive::util::open_path_no_symlinks(source, O_RDONLY)
        );
    } catch (const std::system_error& error) {
        throw LocalUploadResourceError(
            local_resource_code(error.code(), "local_read"),
            "cannot open local upload source '" + source.string() +
                "': " + error.code().message()
        );
    }
    std::filesystem::path snapshot_path;
    onedrive::util::UniqueFileDescriptor output;
    for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
        snapshot_path = upload_snapshot_path(source, attempt);
        try {
            output.reset(onedrive::util::open_path_no_symlinks(
                snapshot_path,
                O_WRONLY | O_CREAT | O_EXCL,
                S_IRUSR | S_IWUSR
            ));
            break;
        } catch (const std::system_error& error) {
            if (!std::filesystem::exists(snapshot_path)) {
                throw LocalUploadResourceError(
                    local_resource_code(error.code(), "local_storage"),
                    "cannot create local upload snapshot '" +
                        snapshot_path.string() + "': " +
                        error.code().message()
                );
            }
        }
    }
    if (!output) {
        throw LocalUploadResourceError(
            "local_storage",
            "cannot allocate a local upload snapshot for: " + source.string()
        );
    }
    UploadSnapshot snapshot{snapshot_path};
    std::array<std::byte, std::size_t{64} * 1024U> buffer{};
    while (true) {
        const auto count = ::read(input.get(), buffer.data(), buffer.size());
        if (count == -1) {
            throw LocalUploadResourceError(
                local_resource_code(
                    std::error_code{errno, std::generic_category()},
                    "local_read"
                ),
                "cannot read local upload source '" + source.string() +
                "': " + std::strerror(errno)
            );
        }
        if (count == 0) {
            break;
        }
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(count)) {
            const auto result = ::write(
                output.get(),
                buffer.data() + written,
                static_cast<std::size_t>(count) - written
            );
            if (result == -1) {
                throw LocalUploadResourceError(
                    local_resource_code(
                        std::error_code{errno, std::generic_category()},
                        "local_storage"
                    ),
                    "cannot write local upload snapshot '" +
                    snapshot_path.string() + "': " + std::strerror(errno)
                );
            }
            written += static_cast<std::size_t>(result);
        }
    }
    if (::fsync(output.get()) == -1) {
        throw LocalUploadResourceError(
            local_resource_code(
                std::error_code{errno, std::generic_category()},
                "local_storage"
            ),
            "cannot flush local upload snapshot '" + snapshot_path.string() +
            "': " + std::strerror(errno)
        );
    }
    output.reset();
    fsync_directory(snapshot_path.parent_path());
    if (!local_file_matches_baseline(source, baseline) ||
        content_fingerprint(snapshot.path()) != baseline.fingerprint) {
        throw LocalModificationConflictError(
            "local file changed while creating its upload snapshot: " +
            source.string()
        );
    }
    return snapshot;
}

std::vector<UploadCandidate> discover_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list,
    std::size_t& blocked,
    const cli::Console& console,
    bool cleanup_suppressions,
    const std::unordered_set<std::string>& skipped_local_paths = {}
) {
    std::unordered_map<std::string, storage::ItemState> tracked;
    for (auto item : items.drive_items(drive_id)) {
        if (cleanup_suppressions) {
            std::error_code identity_error;
            const auto status = std::filesystem::symlink_status(
                item.local_path,
                identity_error
            );
            const bool expected_type =
                !identity_error &&
                (item.directory ?
                     std::filesystem::is_directory(status) :
                     std::filesystem::is_regular_file(status));
            if (expected_type) {
                const auto identity = sync_root.identity(
                    item.local_path,
                    item.directory
                );
                if (item.local_device != identity.device ||
                    item.local_inode != identity.inode) {
                    item.local_device = identity.device;
                    item.local_inode = identity.inode;
                    items.upsert(item);
                }
            } else if (
                identity_error &&
                identity_error !=
                    std::errc::no_such_file_or_directory
            ) {
                throw std::runtime_error(
                    "cannot inspect tracked local identity '" +
                    item.local_path.string() + "': " +
                    identity_error.message()
                );
            }
        }
        tracked.emplace(
            item.local_path.lexically_normal().string(),
            std::move(item)
        );
    }
    std::unordered_set<std::string> blocked_ids;
    std::unordered_set<std::string> blocked_paths;
    for (const auto& item : items.blocked_items(drive_id)) {
        blocked_ids.insert(item.remote_id);
        blocked_paths.insert(item.remote_path);
    }
    std::unordered_map<std::string, storage::PendingUpload>
        resource_blocked_uploads;
    for (auto pending : items.pending_uploads(drive_id)) {
        if (!pending.failure_code.empty() &&
            (!pending.snapshot_path.empty() || pending.directory)) {
            resource_blocked_uploads.emplace(
                pending.local_path.lexically_normal().string(),
                std::move(pending)
            );
        }
    }
    std::unordered_set<std::string> suppressed_paths;
    std::unordered_set<std::string> suppressed_directories;
    for (const auto& suppression : items.upload_suppressions(drive_id)) {
        std::error_code suppression_error;
        const auto status = std::filesystem::symlink_status(
            suppression.local_path,
            suppression_error
        );
        bool matches = false;
        if (!suppression_error &&
            std::filesystem::is_regular_file(status)) {
            const auto identity = sync_root.identity(
                suppression.local_path,
                false
            );
            matches =
                identity.device == suppression.source_device &&
                identity.inode == suppression.source_inode;
        } else if (
            suppression_error &&
            suppression_error !=
                std::errc::no_such_file_or_directory
        ) {
            throw std::runtime_error(
                "cannot inspect selectively retained local file '" +
                suppression.local_path.string() + "': " +
                suppression_error.message()
            );
        }
        if (matches) {
            suppressed_paths.insert(
                suppression.local_path.lexically_normal().string()
            );
            for (auto parent = suppression.local_path.parent_path();
                 parent != sync_root.path() &&
                 parent.lexically_relative(sync_root.path()).
                     native().starts_with("..") == false;
                 parent = parent.parent_path()) {
                suppressed_directories.insert(
                    parent.lexically_normal().string()
                );
            }
        } else if (cleanup_suppressions) {
            items.remove_upload_suppression(
                drive_id,
                suppression.local_path
            );
        }
    }
    std::vector<UploadCandidate> uploads;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator{
        sync_root.path(),
        std::filesystem::directory_options::none,
        error
    };
    if (error) {
        throw std::runtime_error(
            "cannot scan synchronization directory for uploads: " +
            error.message()
        );
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto path = iterator->path();
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect local upload candidate '" + path.string() +
                "': " + error.message()
            );
        }
        if (std::filesystem::is_symlink(status)) {
            ++blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_blocked",
                "Refusing to upload symbolic link '" + path.string() + "'."
            );
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        const bool directory = std::filesystem::is_directory(status);
        const bool regular_file =
            std::filesystem::is_regular_file(status);
        if ((!directory && !regular_file) || reserved_local_name(path)) {
            if (directory) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        const auto relative = path.lexically_relative(sync_root.path());
        const auto remote_path = relative.generic_string();
        if (skipped_local_paths.contains(
                path.lexically_normal().string()
            )) {
            if (directory) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " +
                    error.message()
                );
            }
            continue;
        }
        if (relative.empty() || relative.native().starts_with("..") ||
            (sync_list != nullptr &&
             sync_list->excludes(remote_path, directory))) {
            if (directory) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (sync_list != nullptr &&
            !sync_list->includes(remote_path, directory)) {
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        const auto previous = tracked.find(path.lexically_normal().string());
        if (const auto failed = resource_blocked_uploads.find(
                path.lexically_normal().string()
            );
            failed != resource_blocked_uploads.end()) {
            ++blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_resource_blocked",
                "Upload remains deferred for '" + remote_path + "' (" +
                    failed->second.failure_code + ", attempt " +
                    std::to_string(
                        failed->second.failure_attempt_count
                    ) + "): " + failed->second.failure_message
            );
            if (directory) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " +
                    error.message()
                );
            }
            continue;
        }
        if ((!directory && suppressed_paths.contains(
                 path.lexically_normal().string()
             )) ||
            (directory && suppressed_directories.contains(
                 path.lexically_normal().string()
             ))) {
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (blocked_paths.contains(remote_path) ||
            (previous != tracked.end() &&
             blocked_ids.contains(previous->second.remote_id))) {
            if (directory) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (directory && previous != tracked.end() &&
            !previous->second.directory) {
            ++blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_blocked",
                "Refusing to replace tracked remote file '" +
                    remote_path + "' with a local directory."
            );
            iterator.disable_recursion_pending();
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (previous != tracked.end() && previous->second.directory) {
            if (directory) {
                iterator.increment(error);
                if (error) {
                    throw std::runtime_error(
                        "cannot continue local upload scan: " +
                        error.message()
                    );
                }
                continue;
            }
            ++blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_blocked",
                "Refusing to replace tracked remote directory '" +
                    remote_path + "' with a local file."
            );
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (directory || previous == tracked.end() ||
            !local_snapshot_matches(previous->second, path)) {
            uploads.push_back({
                .path = path,
                .remote_path = remote_path,
                .previous = previous == tracked.end() ?
                    std::nullopt :
                    std::optional{previous->second},
                .directory = directory,
            });
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(
                "cannot continue local upload scan: " + error.message()
            );
        }
    }
    std::ranges::stable_sort(
        uploads,
        [](const UploadCandidate& left, const UploadCandidate& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            if (!left.directory) {
                return false;
            }
            const auto left_depth = std::ranges::distance(left.path);
            const auto right_depth = std::ranges::distance(right.path);
            if (left_depth != right_depth) {
                return left_depth < right_depth;
            }
            return left.remote_path < right.remote_path;
        }
    );
    return uploads;
}

storage::ItemState uploaded_state(
    const graph::RemoteItem& item,
    const std::filesystem::path& local_path,
    const LocalFileBaseline& baseline,
    const std::string& drive_id
) {
    return {
        .drive_id = drive_id,
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .remote_path = item.remote_path,
        .local_path = local_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .local_size = baseline.size,
        .local_modified_ticks = baseline.modified_ticks,
        .directory = false,
    };
}

storage::ItemState uploaded_directory_state(
    const graph::RemoteItem& item,
    const std::filesystem::path& local_path,
    const std::string& drive_id
) {
    if (!item.directory) {
        throw LocalModificationConflictError(
            "remote directory creation returned a file for '" +
            item.remote_path + "'"
        );
    }
    return {
        .drive_id = drive_id,
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .remote_path = item.remote_path,
        .local_path = local_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .local_size = 0,
        .local_modified_ticks = 0,
        .directory = true,
    };
}

void require_local_directory(
    const storage::PendingUpload& upload
) {
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(upload.local_path, error);
    if (error || !std::filesystem::is_directory(status)) {
        throw LocalModificationConflictError(
            "local directory changed during upload: " +
            upload.local_path.string()
        );
    }
}

struct DirectoryUploadTransactionFamily;
using DirectoryUploadTransactionState =
    TransactionState<DirectoryUploadTransactionFamily>;
struct DirectoryUploadPreparedState final : DirectoryUploadTransactionState {};
struct DirectoryUploadJournaledState final : DirectoryUploadTransactionState {};
struct DirectoryUploadGraphCreatedState final
    : DirectoryUploadTransactionState {};
struct DirectoryUploadLocalCommittedState final
    : DirectoryUploadTransactionState {};

struct PendingDirectoryUploadPayload {
    storage::PendingUpload pending;
};

struct GraphCreatedDirectoryUploadPayload {
    storage::PendingUpload pending;
    graph::RemoteItem remote;
};

struct LocalCommittedDirectoryUploadPayload {};

using PreparedDirectoryUpload = StateTransaction<
    DirectoryUploadPreparedState,
    DirectoryUploadTransactionFamily,
    PendingDirectoryUploadPayload>;
using JournaledDirectoryUpload = StateTransaction<
    DirectoryUploadJournaledState,
    DirectoryUploadTransactionFamily,
    PendingDirectoryUploadPayload>;
using GraphCreatedDirectoryUpload = StateTransaction<
    DirectoryUploadGraphCreatedState,
    DirectoryUploadTransactionFamily,
    GraphCreatedDirectoryUploadPayload>;
using LocalCommittedDirectoryUpload = StateTransaction<
    DirectoryUploadLocalCommittedState,
    DirectoryUploadTransactionFamily,
    LocalCommittedDirectoryUploadPayload>;

JournaledDirectoryUpload
journal_directory_upload(PreparedDirectoryUpload transaction) noexcept {
    return transition_transaction<DirectoryUploadJournaledState>(
        std::move(transaction)
    );
}

GraphCreatedDirectoryUpload mark_directory_upload_graph_created(
    JournaledDirectoryUpload transaction, graph::RemoteItem remote
) noexcept {
    return transition_transaction<DirectoryUploadGraphCreatedState>(
        std::move(transaction),
        [remote = std::move(remote)](
            PendingDirectoryUploadPayload&& payload
        ) mutable noexcept {
            return GraphCreatedDirectoryUploadPayload{
                .pending = std::move(payload.pending),
                .remote = std::move(remote),
            };
        }
    );
}

LocalCommittedDirectoryUpload commit_created_directory(
    const SafeSyncRoot& sync_root,
    GraphCreatedDirectoryUpload transaction,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata
) {
    const auto& pending = transaction.pending;
    const auto& remote = transaction.remote;
    require_local_directory(pending);
    if (remote.remote_path != pending.remote_path || !remote.directory) {
        throw std::runtime_error(
            "Microsoft Graph directory response does not match '" +
            pending.remote_path + "'"
        );
    }
    auto state =
        uploaded_directory_state(remote, pending.local_path, pending.drive_id);
    const auto identity = sync_root.identity(pending.local_path, true);
    state.local_device = identity.device;
    state.local_inode = identity.inode;
    items.commit_upload(pending, std::move(state));
    metadata.write_remote_identity(remote, pending.local_path);
    return transition_transaction<DirectoryUploadLocalCommittedState>(
        std::move(transaction),
        [](GraphCreatedDirectoryUploadPayload&&) noexcept {
            return LocalCommittedDirectoryUploadPayload{};
        }
    );
}

template <typename Transaction>
concept JournalableDirectoryUpload = requires(Transaction transaction) {
    journal_directory_upload(std::move(transaction));
};

template <typename Transaction>
concept GraphCreatableDirectoryUpload =
    requires(Transaction transaction, graph::RemoteItem remote) {
        mark_directory_upload_graph_created(
            std::move(transaction), std::move(remote)
        );
    };

template <typename Transaction>
concept LocallyCommittableDirectoryUpload = requires(
    const SafeSyncRoot& sync_root,
    Transaction transaction,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata
) {
    commit_created_directory(
        sync_root, std::move(transaction), items, metadata
    );
};

template <typename Transaction>
concept HasCreatedDirectory =
    requires(Transaction transaction) { transaction.remote.id; };

static_assert(JournalableDirectoryUpload<PreparedDirectoryUpload>);
static_assert(!JournalableDirectoryUpload<JournaledDirectoryUpload>);
static_assert(GraphCreatableDirectoryUpload<JournaledDirectoryUpload>);
static_assert(!GraphCreatableDirectoryUpload<PreparedDirectoryUpload>);
static_assert(LocallyCommittableDirectoryUpload<GraphCreatedDirectoryUpload>);
static_assert(!LocallyCommittableDirectoryUpload<JournaledDirectoryUpload>);
static_assert(!HasCreatedDirectory<PreparedDirectoryUpload>);
static_assert(!HasCreatedDirectory<JournaledDirectoryUpload>);
static_assert(HasCreatedDirectory<GraphCreatedDirectoryUpload>);
static_assert(!HasCreatedDirectory<LocalCommittedDirectoryUpload>);

bool upload_directory(
    const UploadCandidate& upload,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const cli::Console& console,
    UploadSummary& summary
) {
    auto prepared = PreparedDirectoryUpload{
        PendingDirectoryUploadPayload{
            .pending = {
                .drive_id = drive_id,
                .remote_path = upload.remote_path,
                .local_path = upload.path,
                .snapshot_path = {},
                .content_fingerprint = {},
                .local_size = 0,
                .local_modified_ticks = 0,
                .remote_id = std::nullopt,
                .expected_etag = {},
                .upload_url = {},
                .upload_expiration = {},
                .completed_bytes = 0,
                .failure_code = {},
                .failure_message = {},
                .failure_attempt_count = 0,
                .directory = true,
            },
        },
    };
    items.save_pending_upload(prepared.pending);
    auto journaled = journal_directory_upload(std::move(prepared));
    graph::RemoteItem remote;
    try {
        remote = graph.create_directory(upload.remote_path);
    } catch (const graph::UploadConflictError&) {
        items.remove_pending_upload(drive_id, upload.remote_path);
        throw LocalModificationConflictError(
            "remote item conflicts with local directory '" +
            upload.remote_path + "'"
        );
    } catch (const graph::UploadResourceError& error) {
        auto& pending = journaled.pending;
        pending.failure_code = error.reason_code();
        pending.failure_message = error.what();
        pending.failure_attempt_count = 1;
        items.save_pending_upload(pending);
        ++summary.blocked;
        console.message(
            cli::MessageKind::warning,
            "local_upload_resource_blocked",
            "Deferred upload '" + upload.remote_path + "': " +
                error.what()
        );
        return false;
    }
    auto graph_created = mark_directory_upload_graph_created(
        std::move(journaled), std::move(remote)
    );
    static_cast<void>(commit_created_directory(
        sync_root, std::move(graph_created), items, metadata
    ));
    ++summary.created_directories;
    console.message(
        cli::MessageKind::information,
        "local_directory_created",
        "Created remote directory '" + upload.remote_path + "'."
    );
    return true;
}

graph::RemoteItem recover_uploaded_item(
    storage::PendingUpload& upload,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    const auto session = upload.upload_url.empty() ?
        std::nullopt :
        std::optional{graph::UploadSession{
            .upload_url = upload.upload_url,
            .expiration = upload.upload_expiration,
            .completed_bytes = upload.completed_bytes,
        }};
    const graph::UploadCheckpoint checkpoint =
        [&](const graph::UploadSession& state) {
            upload.upload_url = state.upload_url;
            upload.upload_expiration = state.expiration;
            upload.completed_bytes = state.completed_bytes;
            items.save_pending_upload(upload);
        };
    try {
        return graph.upload_file(
            upload.remote_path,
            upload.remote_id,
            upload.expected_etag,
            upload.snapshot_path,
            session,
            checkpoint
        );
    } catch (const graph::UploadConflictError&) {
        const auto remote = graph.item_by_path(upload.remote_path);
        if (remote.size != upload.local_size) {
            throw RemoteUploadConflictError(
                "remote upload recovery conflicts with '" +
                upload.remote_path + "'"
            );
        }
        const auto verification = temporary_path_for(upload.snapshot_path);
        try {
            graph.download_file(
                remote.id,
                remote.etag,
                static_cast<std::uint64_t>(remote.size),
                verification
            );
            const bool matches =
                content_fingerprint(verification) ==
                upload.content_fingerprint;
            static_cast<void>(remove_no_symlinks(verification));
            if (!matches) {
                throw RemoteUploadConflictError(
                    "remote upload recovery content conflicts with '" +
                    upload.remote_path + "'"
                );
            }
        } catch (...) {
            static_cast<void>(remove_no_symlinks(verification));
            throw;
        }
        return remote;
    }
}

GraphCreatedDirectoryUpload recover_created_directory(
    JournaledDirectoryUpload transaction, graph::GraphClient& graph
) {
    const auto& upload = transaction.pending;
    require_local_directory(upload);
    graph::RemoteItem remote;
    try {
        remote = graph.create_directory(upload.remote_path);
    } catch (const graph::UploadConflictError&) {
        remote = graph.item_by_path(upload.remote_path);
        if (!remote.directory) {
            throw RemoteUploadConflictError(
                "remote directory recovery conflicts with '" +
                upload.remote_path + "'"
            );
        }
    }
    return mark_directory_upload_graph_created(
        std::move(transaction), std::move(remote)
    );
}

}  // namespace

void recover_pending_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const cli::Console& console
) {
    for (auto upload : items.pending_uploads(drive_id)) {
        if (!upload.failure_code.empty() && upload.snapshot_path.empty() &&
            !upload.directory) {
            std::error_code status_error;
            const auto status = std::filesystem::symlink_status(
                upload.local_path,
                status_error
            );
            if ((!status_error &&
                 !std::filesystem::is_regular_file(status)) ||
                status_error == std::errc::no_such_file_or_directory) {
                items.remove_pending_upload(
                    upload.drive_id,
                    upload.remote_path
                );
            }
            continue;
        }
        try {
            if (upload.directory) {
                auto journaled = JournaledDirectoryUpload{
                    PendingDirectoryUploadPayload{upload},
                };
                auto graph_created =
                    recover_created_directory(std::move(journaled), graph);
                static_cast<void>(commit_created_directory(
                    sync_root, std::move(graph_created), items, metadata
                ));
                console.message(
                    cli::MessageKind::information,
                    "pending_upload_recovered",
                    "Recovered pending directory creation '" +
                        upload.remote_path + "'."
                );
                continue;
            }
            if (!std::filesystem::is_regular_file(
                    upload.snapshot_path
                ) ||
                content_fingerprint(upload.snapshot_path) !=
                    upload.content_fingerprint) {
                throw std::runtime_error(
                    "pending upload snapshot is missing or changed: " +
                    upload.snapshot_path.string()
                );
            }
            const auto remote =
                recover_uploaded_item(upload, graph, items);
            auto state = uploaded_state(
                remote,
                upload.local_path,
                {
                    .existed = true,
                    .size = upload.local_size,
                    .modified_ticks = upload.local_modified_ticks,
                    .fingerprint = upload.content_fingerprint,
                },
                drive_id
            );
            const auto identity =
                sync_root.identity(upload.local_path, false);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            items.commit_upload(upload, state);
            metadata.write_remote_identity(remote, upload.local_path);
            static_cast<void>(
                remove_no_symlinks(upload.snapshot_path)
            );
            console.message(
                cli::MessageKind::information,
                "pending_upload_recovered",
                "Recovered pending upload '" + upload.remote_path + "'."
            );
        } catch (const graph::UploadResourceError& error) {
            upload.failure_code = error.reason_code();
            upload.failure_message = error.what();
            ++upload.failure_attempt_count;
            items.save_pending_upload(upload);
            console.message(
                cli::MessageKind::warning,
                "pending_upload_resource_blocked",
                "Deferred pending upload '" + upload.remote_path + "': " +
                    error.what()
            );
        } catch (const RemoteUploadConflictError& error) {
            items.remove_pending_upload(
                upload.drive_id,
                upload.remote_path
            );
            if (!upload.snapshot_path.empty()) {
                static_cast<void>(
                    remove_no_symlinks(upload.snapshot_path)
                );
            }
            console.message(
                cli::MessageKind::warning,
                "pending_upload_conflict_deferred",
                std::string{error.what()} +
                    "; reconciling it through the remote delta."
            );
        }
    }
}

void recover_pending_deletes(
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console,
    RemoteDeletionPolicy deletion_policy
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
        enforce_remote_deletion_limit(plan, deletion_policy, console, false)
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

void recover_pending_remote_moves(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console
) {
    for (const auto& move : items.pending_remote_moves(drive_id)) {
        const auto previous = items.find(drive_id, move.remote_id);
        if (!previous) {
            throw std::runtime_error(
                "pending remote move has no tracked item: " +
                move.remote_id
            );
        }
        auto journaled = JournaledRemoteMove{
            PendingRemoteMovePayload{
                .move = move,
                .previous = *previous,
            },
        };
        static_cast<void>(execute_pending_remote_move(
            sync_root, std::move(journaled), graph, items
        ));
        console.message(
            cli::MessageKind::information,
            "pending_remote_move_recovered",
            "Recovered remote move to '" +
                move.destination_remote_path + "'."
        );
    }
}

UploadSummary upload_local_changes(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const SyncList* sync_list,
    const cli::Console& console,
    bool dry_run,
    RemoteDeletionPolicy deletion_policy
) {
    UploadSummary summary;
    auto moves = discover_local_moves(
        sync_root,
        drive_id,
        items,
        sync_list
    );
    summary.planned_moves = moves.moves.size();
    auto deletion_plan =
        discover_deletions(drive_id, items, sync_list, moves.moved_remote_ids);
    summary.planned_deletions = deletion_plan.operations.size();
    summary.affected_deletions = deletion_plan.affected_items;
    summary.large_delete_blocked = enforce_remote_deletion_limit(
        deletion_plan, deletion_policy, console, dry_run
    );
    if (dry_run) {
        const auto uploads = discover_uploads(
            sync_root,
            drive_id,
            items,
            sync_list,
            summary.blocked,
            console,
            false,
            moves.moved_local_paths
        );
        summary.planned = static_cast<std::size_t>(
            std::ranges::count(
                uploads,
                false,
                &UploadCandidate::directory
            )
        );
        summary.planned_directories =
            uploads.size() - summary.planned;
        return summary;
    }

    auto move_parents = discover_move_parent_uploads(
        sync_root,
        drive_id,
        items,
        moves.moves
    );
    summary.planned_directories = move_parents.size();
    for (const auto& parent : move_parents) {
        if (!upload_directory(
            parent,
            sync_root,
            drive_id,
            graph,
            items,
            metadata,
            console,
            summary
        )) {
            return summary;
        }
    }
    for (const auto& move : moves.moves) {
        const auto previous = items.find(drive_id, move.remote_id);
        if (!previous) {
            throw std::runtime_error(
                "local move has no tracked item: " + move.remote_id
            );
        }
        auto prepared = PreparedRemoteMove{
            PendingRemoteMovePayload{
                .move = move,
                .previous = *previous,
            },
        };
        items.save_pending_remote_move(prepared.move);
        auto journaled = journal_remote_move(std::move(prepared));
        static_cast<void>(execute_pending_remote_move(
            sync_root, std::move(journaled), graph, items
        ));
        moves.moved_remote_ids.erase(move.remote_id);
        ++summary.moved;
        console.message(
            cli::MessageKind::information,
            "local_move_uploaded",
            "Moved remote item to '" +
                move.destination_remote_path + "'."
        );
    }
    deletion_plan =
        discover_deletions(drive_id, items, sync_list, moves.moved_remote_ids);
    summary.planned_deletions = deletion_plan.operations.size();
    summary.affected_deletions = deletion_plan.affected_items;
    summary.large_delete_blocked = enforce_remote_deletion_limit(
        deletion_plan, deletion_policy, console, false
    );
    auto uploads = discover_uploads(
        sync_root,
        drive_id,
        items,
        sync_list,
        summary.blocked,
        console,
        true
    );
    summary.planned = static_cast<std::size_t>(std::ranges::count(
        uploads,
        false,
        &UploadCandidate::directory
    ));
    summary.planned_directories +=
        uploads.size() - summary.planned;
    std::unordered_map<std::string, std::uint64_t>
        previous_failure_attempts;
    for (const auto& pending : items.pending_uploads(drive_id)) {
        if (!pending.failure_code.empty()) {
            previous_failure_attempts.emplace(
                pending.remote_path,
                pending.failure_attempt_count
            );
        }
    }
    for (const auto& deletion : deletion_plan.operations) {
        auto prepared = PreparedRemoteDelete{
            PendingRemoteDeletePayload{deletion},
        };
        items.save_pending_delete(prepared.deletion);
        auto journaled = journal_remote_delete(std::move(prepared));
        static_cast<void>(
            execute_pending_delete(std::move(journaled), graph, items)
        );
        ++summary.deleted;
        console.message(
            cli::MessageKind::information,
            "local_item_deleted",
            "Deleted remote item '" + deletion.remote_path + "'."
        );
    }
    for (const auto& upload : uploads) {
        if (upload.directory) {
            static_cast<void>(upload_directory(
                upload,
                sync_root,
                drive_id,
                graph,
                items,
                metadata,
                console,
                summary
            ));
            continue;
        }
        storage::PendingUpload pending{
            .drive_id = drive_id,
            .remote_path = upload.remote_path,
            .local_path = upload.path,
            .snapshot_path = {},
            .content_fingerprint = {},
            .local_size = 0,
            .local_modified_ticks = 0,
            .remote_id = upload.previous ?
                std::optional{upload.previous->remote_id} :
                std::nullopt,
            .expected_etag = upload.previous ? upload.previous->etag : "",
            .upload_url = {},
            .upload_expiration = {},
            .completed_bytes = 0,
            .failure_code = {},
            .failure_message = {},
            .failure_attempt_count = 0,
            .directory = false,
        };
        try {
            const auto baseline =
                capture_local_file_baseline(upload.path);
            if (!baseline.existed) {
                continue;
            }
            auto snapshot = create_upload_snapshot(upload.path, baseline);
            pending.snapshot_path = snapshot.path();
            pending.content_fingerprint = baseline.fingerprint;
            pending.local_size = baseline.size;
            pending.local_modified_ticks = baseline.modified_ticks;
            auto prepared = PreparedUpload{PreparedUploadPayload{
                .pending = &pending,
                .baseline = baseline,
                .snapshot = std::move(snapshot),
            }};
            items.save_pending_upload(pending_upload(prepared));
            auto journaled = journal_upload(std::move(prepared));
            const auto checkpoint =
                persist_upload_checkpoints(journaled, items);
            auto remote = graph.upload_file(
                upload.remote_path,
                pending.remote_id,
                pending.expected_etag,
                pending.snapshot_path,
                std::nullopt,
                checkpoint
            );
            if (remote.remote_path != upload.remote_path ||
                remote.size != baseline.size) {
                throw std::runtime_error(
                    "Microsoft Graph upload response does not match local "
                    "file '" + upload.path.string() + "'"
                );
            }
            auto remote_committed = mark_remote_upload_committed(
                std::move(journaled), std::move(remote)
            );
            auto state = uploaded_state(
                remote_committed.remote,
                upload.path,
                remote_committed.baseline,
                drive_id
            );
            const auto identity = sync_root.identity(upload.path, false);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            items.commit_upload(
                pending_upload(remote_committed), std::move(state)
            );
            metadata.write_remote_identity(
                remote_committed.remote, upload.path
            );
            static_cast<void>(remove_no_symlinks(
                pending_upload(remote_committed).snapshot_path
            ));
            ++summary.uploaded;
            console.message(
                cli::MessageKind::information,
                "local_item_uploaded",
                "Uploaded local file '" + upload.remote_path + "'."
            );
        } catch (const LocalUploadResourceError& error) {
            pending.failure_code = error.reason_code();
            pending.failure_message = error.what();
            pending.failure_attempt_count =
                previous_failure_attempts[upload.remote_path] + 1;
            items.save_pending_upload(pending);
            ++summary.blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_resource_blocked",
                "Deferred upload '" + upload.remote_path + "': " +
                    error.what()
            );
        } catch (const graph::UploadResourceError& error) {
            pending.failure_code = error.reason_code();
            pending.failure_message = error.what();
            ++pending.failure_attempt_count;
            items.save_pending_upload(pending);
            ++summary.blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_resource_blocked",
                "Deferred upload '" + upload.remote_path + "': " +
                    error.what()
            );
        } catch (const std::system_error& error) {
            pending.failure_code =
                local_resource_code(error.code(), "local_read");
            pending.failure_message = error.what();
            pending.failure_attempt_count =
                previous_failure_attempts[upload.remote_path] + 1;
            items.save_pending_upload(pending);
            ++summary.blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_resource_blocked",
                "Deferred upload '" + upload.remote_path + "': " +
                    error.what()
            );
        }
    }
    return summary;
}

}  // namespace onedrive::sync::detail
