#include "sync/upload/remote_move.hpp"
#include "sync/upload/planning.hpp"
#include "onedrive/cli/console.hpp"
#include "util/typestate.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

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

LocalMoveDiscovery discover_local_moves(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list
) {
    struct CurrentItem {
        std::filesystem::path path;
        std::string remote_path;
        std::optional<std::uint64_t> size;
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
            std::optional<std::uint64_t> size;
            if (regular) {
                size = iterator->file_size(error);
                if (error) {
                    throw std::runtime_error(
                        "cannot read local move candidate size '" +
                        path.string() + "': " + error.message()
                    );
                }
            }
            const auto identity = sync_root.identity(path, directory);
            const auto key = identity_key(
                identity.device,
                identity.inode
            );
            if (!current.emplace(
                    key,
                    CurrentItem{path, remote_path, size, directory}
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
                 item.directory ?
                     SyncItemKind::directory :
                     SyncItemKind::file,
                 found->second.size
             ) ||
             !sync_list->includes(
                 found->second.remote_path,
                 item.directory ?
                     SyncItemKind::directory :
                     SyncItemKind::file,
                 found->second.size
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
struct RemoteMoveTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, RemoteMovePreparedState> &&
                std::same_as<Next, RemoteMoveJournaledState>) ||
               (std::same_as<Current, RemoteMoveJournaledState> &&
                std::same_as<Next, RemoteMoveGraphCommittedState>) ||
               (std::same_as<Current, RemoteMoveGraphCommittedState> &&
                std::same_as<Next, RemoteMoveLocalCommittedState>);
    }
};

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

void execute_new_remote_move(
    const SafeSyncRoot& sync_root,
    storage::PendingRemoteMove move,
    storage::ItemState previous,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    auto prepared = PreparedRemoteMove{
        PendingRemoteMovePayload{
            .move = std::move(move),
            .previous = std::move(previous),
        },
    };
    items.save_pending_remote_move(prepared.move);
    auto journaled = journal_remote_move(std::move(prepared));
    static_cast<void>(execute_pending_remote_move(
        sync_root, std::move(journaled), graph, items
    ));
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

}  // namespace onedrive::sync::detail
