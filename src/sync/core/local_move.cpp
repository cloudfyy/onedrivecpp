#include "sync/core/local_move.hpp"
#include "sync/core/move_schedule.hpp"

#include "onedrive/ui/common/observer.hpp"
#include "sync/core/item_ops.hpp"
#include "sync/core/reporting.hpp"
#include "sync/core/plan.hpp"
#include "sync/download/integrity.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "util/typestate.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace onedrive::sync::engine_detail {
namespace {

struct LocalMoveTransactionFamily;
using LocalMoveTransactionState =
    util::TransactionState<LocalMoveTransactionFamily>;
struct LocalMovePreparedState final : LocalMoveTransactionState {};
struct LocalMoveJournaledState final : LocalMoveTransactionState {};
struct LocalMoveRecoveredJournalState final : LocalMoveTransactionState {};
struct LocalMoveStagedState final : LocalMoveTransactionState {};
struct LocalMoveInstalledState final : LocalMoveTransactionState {};
struct LocalMoveTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, LocalMovePreparedState> &&
                std::same_as<Next, LocalMoveJournaledState>) ||
               ((std::same_as<Current, LocalMoveJournaledState> ||
                 std::same_as<Current, LocalMoveRecoveredJournalState>) &&
                std::same_as<Next, LocalMoveStagedState>) ||
               ((std::same_as<Current, LocalMoveJournaledState> ||
                 std::same_as<Current, LocalMoveStagedState>) &&
                std::same_as<Next, LocalMoveInstalledState>);
    }
};

template <typename State>
concept LocalMoveState =
    util::TransactionStateFor<State, LocalMoveTransactionFamily>;

struct LocalMoveTransactionPayload {
    storage::PendingMove journal;
};

template <LocalMoveState State>
using LocalMoveTransaction = util::StateTransaction<
    State,
    LocalMoveTransactionFamily,
    LocalMoveTransactionPayload>;

using PreparedLocalMove = LocalMoveTransaction<LocalMovePreparedState>;
using JournaledLocalMove = LocalMoveTransaction<LocalMoveJournaledState>;
using RecoveredJournalLocalMove =
    LocalMoveTransaction<LocalMoveRecoveredJournalState>;
using StagedLocalMove = LocalMoveTransaction<LocalMoveStagedState>;
using InstalledLocalMove = LocalMoveTransaction<LocalMoveInstalledState>;

static_assert(std::is_nothrow_move_constructible_v<PreparedLocalMove>);
static_assert(std::is_nothrow_move_constructible_v<JournaledLocalMove>);
static_assert(std::is_nothrow_move_constructible_v<StagedLocalMove>);
static_assert(std::is_nothrow_move_constructible_v<InstalledLocalMove>);

template <typename... States>
[[nodiscard]] bool has_discardable_move_journal(
    const std::variant<States...>& transaction
) noexcept {
    return std::holds_alternative<JournaledLocalMove>(transaction);
}

template <typename... States>
[[nodiscard]] bool has_discardable_move_journal(
    const std::optional<std::variant<States...>>& transaction
) noexcept {
    return transaction.has_value() &&
           has_discardable_move_journal(*transaction);
}

JournaledLocalMove journal_local_move(PreparedLocalMove move) noexcept {
    return util::transition_transaction<LocalMoveJournaledState>(std::move(move)
    );
}

StagedLocalMove stage_local_move(JournaledLocalMove move) noexcept {
    return util::transition_transaction<LocalMoveStagedState>(std::move(move));
}

StagedLocalMove stage_local_move(RecoveredJournalLocalMove move) noexcept {
    return util::transition_transaction<LocalMoveStagedState>(std::move(move));
}

InstalledLocalMove install_local_move(JournaledLocalMove move) noexcept {
    return util::transition_transaction<LocalMoveInstalledState>(std::move(move)
    );
}

InstalledLocalMove install_local_move(StagedLocalMove move) noexcept {
    return util::transition_transaction<LocalMoveInstalledState>(std::move(move)
    );
}

template <typename Transaction>
concept JournalableLocalMove = requires(Transaction transaction) {
    journal_local_move(std::move(transaction));
};

template <typename Transaction>
concept StageableLocalMove = requires(Transaction transaction) {
    stage_local_move(std::move(transaction));
};

template <typename Transaction>
concept InstallableLocalMove = requires(Transaction transaction) {
    install_local_move(std::move(transaction));
};

static_assert(JournalableLocalMove<PreparedLocalMove>);
static_assert(!JournalableLocalMove<JournaledLocalMove>);
static_assert(!JournalableLocalMove<StagedLocalMove>);
static_assert(!JournalableLocalMove<InstalledLocalMove>);
static_assert(StageableLocalMove<JournaledLocalMove>);
static_assert(StageableLocalMove<RecoveredJournalLocalMove>);
static_assert(!StageableLocalMove<PreparedLocalMove>);
static_assert(!StageableLocalMove<InstalledLocalMove>);
static_assert(InstallableLocalMove<JournaledLocalMove>);
static_assert(InstallableLocalMove<StagedLocalMove>);
static_assert(!InstallableLocalMove<PreparedLocalMove>);
static_assert(!InstallableLocalMove<InstalledLocalMove>);

bool remote_file_content_unchanged(
    const graph::RemoteItem& item,
    const storage::ItemState& previous,
    const std::filesystem::path& path
) {
    if (detail::has_enriched_remote_size(previous)) {
        return item.size == previous.size &&
               item.last_modified == previous.last_modified &&
               detail::remote_content_version_matches(
                   previous, item.etag, item.ctag
               );
    }
    if (!item.content_hash) {
        return item.size == previous.size &&
               item.last_modified == previous.last_modified;
    }
    return detail::file_matches_content_hash(path, *item.content_hash);
}

struct DirectoryMove {
    std::filesystem::path source;
    std::filesystem::path destination;
};

class MovePathRemapper {
public:
    [[nodiscard]] std::filesystem::path remap(
        std::filesystem::path source
    ) const {
        for (const auto& directory : completed_directory_moves_) {
            const auto relative =
                source.lexically_relative(directory.source);
            if (relative.empty()) {
                source = directory.destination;
            } else if (!relative.native().starts_with("..") &&
                       !relative.is_absolute()) {
                source = directory.destination / relative;
            }
        }
        return source;
    }

    void record(
        std::filesystem::path source,
        std::filesystem::path destination
    ) {
        completed_directory_moves_.push_back({
            .source = std::move(source),
            .destination = std::move(destination),
        });
    }

private:
    std::vector<DirectoryMove> completed_directory_moves_;
};

struct MoveExecutionContext {
    detail::SyncPlan& plan;
    const detail::SafeSyncRoot& safe_root;
    const std::string& drive_id;
    storage::ItemStore& items;
    detail::ItemOperationCoordinator& operations;
    const events::Observer& observer;
    config::SyncPermissionsMode permissions;
    const std::vector<LocalMoveCandidate>& moves;
    std::unordered_map<std::string, storage::PendingMove>& pending_moves;
    const MoveSchedule& schedule;
    MoveSummary& summary;
    MovePathRemapper& paths;
    const std::unordered_set<std::string>& recovered_moves;

    void block(
        const LocalMoveCandidate& move,
        std::string code,
        std::string message
    ) {
        plan.block(move.item, std::move(code), std::move(message));
        summary.blocked.insert(move.item.id);
        report_blocked(plan.blocked(plan.blocked_count() - 1), observer);
    }
};

void execute_staging_action(
    std::size_t move_index,
    MoveExecutionContext& context
) {
    const auto& move = context.moves[move_index];
    const auto source = context.paths.remap(move.previous.local_path);
    const auto pending_iterator =
        context.pending_moves.find(move.item.id);
    const storage::PendingMove* saved_pending =
        pending_iterator == context.pending_moves.end() ?
            nullptr :
            &pending_iterator->second;
    using ActiveStagingMove = std::variant<
        JournaledLocalMove,
        RecoveredJournalLocalMove,
        StagedLocalMove>;
    std::optional<ActiveStagingMove> move_transaction;
    const auto block = [&](std::string code, std::string message) {
        context.block(move, std::move(code), std::move(message));
    };
    const auto discard_journal = [&] {
        if (!has_discardable_move_journal(move_transaction)) {
            return false;
        }
        context.items.remove_pending_move(context.drive_id, move.item.id);
        context.pending_moves.erase(move.item.id);
        return true;
    };
    try {
        auto operation = context.operations.acquire(
            context.drive_id, move.previous.remote_id
        );
        auto source_operation =
            context.operations.acquire_destination(source);
        static_cast<void>(context.safe_root.relative_path(source));
        static_cast<void>(
            context.safe_root.relative_path(move.destination)
        );
        const auto expected_type = [&](auto status) {
            return !std::filesystem::is_symlink(status) &&
                   (move.previous.directory ?
                        std::filesystem::is_directory(status) :
                        std::filesystem::is_regular_file(status));
        };
        const auto identity_matches = [&](const auto& path,
                                          const auto& pending) {
            std::error_code error;
            const auto status =
                std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory) {
                return false;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot inspect pending move path '" + path.string() +
                    "': " + error.message()
                );
            }
            if (!std::filesystem::exists(status) ||
                !expected_type(status)) {
                return false;
            }
            const auto identity = context.safe_root.identity(
                path,
                detail::filesystem_item_kind(move.previous.directory)
            );
            return identity.device == pending.source_device &&
                   identity.inode == pending.source_inode;
        };

        std::filesystem::path staging;
        if (saved_pending != nullptr) {
            if (saved_pending->source_path != source ||
                saved_pending->destination_path != move.destination ||
                saved_pending->staging_path.empty() ||
                saved_pending->directory != move.previous.directory) {
                block(
                    "pending_move_conflict",
                    "saved staging journal does not match the current "
                    "remote move for: " + move.item.remote_path
                );
                return;
            }
            staging = saved_pending->staging_path;
            static_cast<void>(context.safe_root.relative_path(staging));
            if (staging == source || staging == move.destination) {
                block(
                    "pending_move_conflict",
                    "saved staging path overlaps a move endpoint: " +
                        staging.string()
                );
                return;
            }
            if (identity_matches(move.destination, *saved_pending)) {
                if (move.previous.directory) {
                    context.paths.record(source, move.destination);
                }
                return;
            }
            if (identity_matches(staging, *saved_pending)) {
                if (move.previous.directory) {
                    context.paths.record(source, staging);
                }
                return;
            }
            if (!identity_matches(source, *saved_pending)) {
                block(
                    "pending_move_conflict",
                    "staged move object does not match its durable "
                    "filesystem identity: " + move.item.remote_path
                );
                return;
            }
            move_transaction.emplace(
                std::in_place_type<RecoveredJournalLocalMove>,
                RecoveredJournalLocalMove{
                    LocalMoveTransactionPayload{*saved_pending},
                }
            );
        } else {
            std::error_code source_error;
            const auto source_status =
                std::filesystem::symlink_status(source, source_error);
            if (source_error ||
                !std::filesystem::exists(source_status) ||
                !expected_type(source_status)) {
                block(
                    "local_path_conflict",
                    "remote move staging source is missing or has an "
                    "unexpected type: " + source.string()
                );
                return;
            }
            if (!move.previous.directory &&
                !detail::local_snapshot_matches(move.previous, source)) {
                block(
                    "local_modification",
                    "local file changed before staging remote move: " +
                        source.string()
                );
                return;
            }
            constexpr std::size_t maximum_staging_path_attempts = 100;
            for (std::size_t attempt = 0;
                 attempt < maximum_staging_path_attempts; ++attempt) {
                const auto candidate =
                    detail::move_staging_path_for(source);
                std::error_code candidate_error;
                const auto candidate_status =
                    std::filesystem::symlink_status(
                        candidate, candidate_error
                    );
                if (candidate_error ==
                    std::errc::no_such_file_or_directory) {
                    staging = candidate;
                    break;
                }
                if (candidate_error) {
                    throw std::runtime_error(
                        "cannot inspect move staging path '" +
                        candidate.string() + "': " +
                        candidate_error.message()
                    );
                }
                if (!std::filesystem::exists(candidate_status)) {
                    staging = candidate;
                    break;
                }
            }
            if (staging.empty()) {
                throw std::runtime_error(
                    "cannot allocate a private move staging path for: " +
                    source.string()
                );
            }
            const auto identity = context.safe_root.identity(
                source,
                detail::filesystem_item_kind(move.previous.directory)
            );
            storage::PendingMove pending{
                .drive_id = context.drive_id,
                .remote_id = move.item.id,
                .source_path = source,
                .destination_path = move.destination,
                .staging_path = staging,
                .source_device = identity.device,
                .source_inode = identity.inode,
                .directory = move.previous.directory,
            };
            auto prepared = PreparedLocalMove{
                LocalMoveTransactionPayload{pending},
            };
            context.items.save_pending_move(prepared.journal);
            context.pending_moves.insert_or_assign(
                move.item.id, prepared.journal
            );
            move_transaction.emplace(
                std::in_place_type<JournaledLocalMove>,
                journal_local_move(std::move(prepared))
            );
        }

        auto staging_operation =
            context.operations.acquire_destination(staging);
        if (!context.safe_root.rename_no_replace(source, staging)) {
            if (discard_journal()) {
                move_transaction.reset();
            }
            block(
                "local_path_conflict",
                "private move staging path already exists: " +
                    staging.string()
            );
            return;
        }
        auto staged = std::visit(
            [](auto&& transaction) -> StagedLocalMove {
                using Transaction =
                    std::remove_cvref_t<decltype(transaction)>;
                if constexpr (std::same_as<Transaction, StagedLocalMove>) {
                    return std::forward<decltype(transaction)>(transaction);
                } else {
                    return stage_local_move(
                        std::forward<decltype(transaction)>(transaction)
                    );
                }
            },
            std::move(*move_transaction)
        );
        move_transaction.emplace(
            std::in_place_type<StagedLocalMove>, std::move(staged)
        );
        context.safe_root.fsync_directory(source.parent_path());
        if (move.previous.directory) {
            context.paths.record(source, staging);
        }
        context.observer.message(
            events::MessageKind::information,
            "local_item_move_staged",
            "Staged remotely moved item from '" + source.string() + "' at '" +
                staging.string() + "'."
        );
    } catch (const detail::CrossDeviceMoveError& error) {
        static_cast<void>(discard_journal());
        block("cross_device_move", error.what());
    } catch (const detail::SafePathConflictError& error) {
        static_cast<void>(discard_journal());
        block("local_path_conflict", error.what());
    } catch (const detail::LocalPathConflictError& error) {
        static_cast<void>(discard_journal());
        block("local_path_conflict", error.what());
    }
}

void execute_install_action(
    std::size_t move_index,
    MoveExecutionContext& context
) {
    const auto& move = context.moves[move_index];
    const auto failed_prerequisite = std::ranges::find_if(
        context.schedule.dependencies[move_index],
        [&](std::size_t prerequisite) {
            return context.summary.blocked.contains(
                context.moves[prerequisite].item.id
            );
        }
    );
    if (failed_prerequisite !=
        context.schedule.dependencies[move_index].end()) {
        context.block(
            move,
            "move_dependency_blocked",
            "remote move depends on another move that could not be "
            "applied: " +
                context.moves[*failed_prerequisite].item.remote_path
        );
        return;
    }
    auto& state = context.plan.state_for(move.item.id);
    const auto& destination = move.destination;
    const auto pending_iterator =
        context.pending_moves.find(move.item.id);
    const storage::PendingMove* pending =
        pending_iterator == context.pending_moves.end() ?
            nullptr :
            &pending_iterator->second;
    const auto dependency_source =
        context.paths.remap(move.previous.local_path);
    const auto source =
        pending != nullptr && !pending->staging_path.empty() ?
            pending->staging_path :
            dependency_source;
    const bool staged_journal =
        pending != nullptr && !pending->staging_path.empty();
    using ActiveLocalMove = std::variant<
        PreparedLocalMove,
        JournaledLocalMove,
        StagedLocalMove,
        InstalledLocalMove>;
    ActiveLocalMove move_transaction =
        pending == nullptr ?
            ActiveLocalMove{std::in_place_type<PreparedLocalMove>} :
        staged_journal ?
            ActiveLocalMove{
                std::in_place_type<StagedLocalMove>,
                StagedLocalMove{
                    LocalMoveTransactionPayload{*pending},
                },
            } :
            ActiveLocalMove{
                std::in_place_type<JournaledLocalMove>,
                JournaledLocalMove{
                    LocalMoveTransactionPayload{*pending},
                },
            };
    const auto mark_installed = [&] {
        auto installed = std::visit(
            [](auto&& transaction) -> InstalledLocalMove {
                using Transaction =
                    std::remove_cvref_t<decltype(transaction)>;
                if constexpr (std::same_as<
                                  Transaction,
                                  JournaledLocalMove> ||
                              std::same_as<Transaction, StagedLocalMove>) {
                    return install_local_move(
                        std::forward<decltype(transaction)>(transaction)
                    );
                } else if constexpr (std::same_as<
                                         Transaction,
                                         InstalledLocalMove>) {
                    return std::forward<decltype(transaction)>(transaction);
                } else {
                    throw std::logic_error(
                        "local move was installed without a journal"
                    );
                }
            },
            std::move(move_transaction)
        );
        move_transaction = std::move(installed);
    };
    const auto block = [&](std::string code, std::string message) {
        context.block(move, std::move(code), std::move(message));
    };
    const auto discard_journal = [&] {
        if (!has_discardable_move_journal(move_transaction)) {
            return false;
        }
        context.items.remove_pending_move(context.drive_id, move.item.id);
        return true;
    };
    try {
        if (pending != nullptr &&
            ((!staged_journal &&
              pending->source_path != dependency_source) ||
             pending->destination_path != destination ||
             pending->directory != move.previous.directory)) {
            block(
                "pending_move_conflict",
                "saved move journal does not match the current remote "
                "move for: " + move.item.remote_path
            );
            return;
        }
        auto operation = context.operations.acquire(
            context.drive_id, move.previous.remote_id
        );
        auto source_operation =
            context.operations.acquire_destination(source);
        std::optional<detail::ItemOperationCoordinator::Lease>
            destination_operation;
        if (source != destination) {
            destination_operation.emplace(
                context.operations.acquire_destination(destination)
            );
        }
        static_cast<void>(context.safe_root.relative_path(source));
        static_cast<void>(context.safe_root.relative_path(destination));

        std::error_code source_error;
        const auto source_status =
            std::filesystem::symlink_status(source, source_error);
        const bool source_exists =
            !source_error && std::filesystem::exists(source_status);
        if (source_error &&
            source_error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "cannot inspect local move source '" + source.string() +
                "': " + source_error.message()
            );
        }

        std::error_code destination_error;
        const auto destination_status = std::filesystem::symlink_status(
            destination, destination_error
        );
        const bool destination_exists =
            !destination_error &&
            std::filesystem::exists(destination_status);
        if (destination_error &&
            destination_error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "cannot inspect local move destination '" +
                destination.string() + "': " +
                destination_error.message()
            );
        }

        const auto expected_type = [&](auto status) {
            return !std::filesystem::is_symlink(status) &&
                   (move.previous.directory ?
                        std::filesystem::is_directory(status) :
                        std::filesystem::is_regular_file(status));
        };
        bool pending_destination_matches = false;
        if (pending != nullptr && destination_exists &&
            expected_type(destination_status)) {
            const auto identity = context.safe_root.identity(
                destination,
                detail::filesystem_item_kind(move.previous.directory)
            );
            pending_destination_matches =
                identity.device == pending->source_device &&
                identity.inode == pending->source_inode;
            if (pending_destination_matches) {
                mark_installed();
            }
        }
        if (!pending_destination_matches && source == destination) {
            if (!source_exists ||
                !expected_type(source_status) ||
                (!move.previous.directory &&
                 !detail::local_snapshot_matches(move.previous, source))) {
                block(
                    "local_path_conflict",
                    "remotely moved item is not present at its "
                    "dependency-adjusted destination: " +
                        destination.string()
                );
                return;
            }
            if (pending != nullptr) {
                block(
                    "pending_move_conflict",
                    "remote move destination does not match the durable "
                    "source identity: " + destination.string()
                );
                return;
            }
        } else if (!pending_destination_matches && !source_exists) {
            if (!destination_exists ||
                !expected_type(destination_status) ||
                (!move.previous.directory &&
                 !detail::local_snapshot_matches(
                     move.previous, destination
                 ))) {
                block(
                    "local_path_conflict",
                    "remote move source is missing and its destination "
                    "cannot be safely adopted: " + source.string()
                );
                return;
            }
            if (pending != nullptr) {
                const auto identity = context.safe_root.identity(
                    destination,
                    detail::filesystem_item_kind(move.previous.directory)
                );
                if (identity.device != pending->source_device ||
                    identity.inode != pending->source_inode) {
                    block(
                        "pending_move_conflict",
                        "remote move destination does not match the "
                        "durable source identity: " +
                            destination.string()
                    );
                    return;
                }
            }
        } else if (!pending_destination_matches) {
            if (!expected_type(source_status)) {
                block(
                    "local_path_conflict",
                    "remote move source has an unexpected local type: " +
                        source.string()
                );
                return;
            }
            if (!move.previous.directory &&
                !detail::local_snapshot_matches(move.previous, source)) {
                block(
                    "local_modification",
                    "local file changed before applying remote move: " +
                        source.string()
                );
                return;
            }
            if (destination_exists) {
                block(
                    "local_path_conflict",
                    "remote move destination already exists: " +
                        destination.string()
                );
                return;
            }
            context.safe_root.ensure_directory_tree(
                destination.parent_path(), context.permissions
            );
            const auto source_identity = context.safe_root.identity(
                source,
                detail::filesystem_item_kind(move.previous.directory)
            );
            if (pending != nullptr &&
                (source_identity.device != pending->source_device ||
                 source_identity.inode != pending->source_inode)) {
                block(
                    "pending_move_conflict",
                    "remote move source no longer matches its durable "
                    "filesystem identity: " + source.string()
                );
                return;
            }
            if (pending == nullptr) {
                auto& prepared =
                    std::get<PreparedLocalMove>(move_transaction);
                prepared.journal = {
                    .drive_id = context.drive_id,
                    .remote_id = move.item.id,
                    .source_path = source,
                    .destination_path = destination,
                    .staging_path = {},
                    .source_device = source_identity.device,
                    .source_inode = source_identity.inode,
                    .directory = move.previous.directory,
                };
                context.items.save_pending_move(prepared.journal);
                move_transaction.emplace<JournaledLocalMove>(
                    journal_local_move(std::move(prepared))
                );
            }
            if (!context.safe_root.rename_no_replace(source, destination)) {
                if (discard_journal()) {
                    move_transaction.emplace<PreparedLocalMove>();
                }
                block(
                    "local_path_conflict",
                    "remote move destination already exists: " +
                        destination.string()
                );
                return;
            }
            mark_installed();
            context.safe_root.fsync_directory(source.parent_path());
            if (source.parent_path() != destination.parent_path()) {
                context.safe_root.fsync_directory(
                    destination.parent_path()
                );
            }
            ++context.summary.moved;
            context.observer.file_operation({
                .action = events::FileAction::moved_locally,
                .path = move.item.remote_path,
                .previous_path = move.previous.remote_path,
                .drive_id = context.drive_id,
                .local_path = destination.string(),
                .directory = move.previous.directory,
                .recovered = context.recovered_moves.contains(move.item.id),
            });
            context.observer.message(
                events::MessageKind::information,
                "local_item_moved",
                "Moved remotely renamed item from '" + source.string() +
                    "' to '" + destination.string() + "'."
            );
        }

        if (move.previous.directory && source != destination) {
            context.paths.record(source, destination);
        }
        if (!move.previous.directory) {
            state.local_size = detail::persisted_file_size(
                std::filesystem::file_size(destination)
            );
            state.local_modified_ticks =
                detail::modified_ticks(destination);
            if (remote_file_content_unchanged(
                    move.item, move.previous, destination
                )) {
                detail::preserve_local_content_hash(state, move.previous);
                context.summary.reusable_files.insert(move.item.id);
            }
        }
        const auto identity = context.safe_root.identity(
            destination,
            detail::filesystem_item_kind(move.previous.directory)
        );
        state.local_device = identity.device;
        state.local_inode = identity.inode;
    } catch (const detail::CrossDeviceMoveError& error) {
        static_cast<void>(discard_journal());
        block("cross_device_move", error.what());
    } catch (const detail::SafePathConflictError& error) {
        static_cast<void>(discard_journal());
        block("local_path_conflict", error.what());
    } catch (const detail::LocalPathConflictError& error) {
        static_cast<void>(discard_journal());
        block("local_path_conflict", error.what());
    }
}

}  // namespace

MoveSummary execute_moves(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const events::Observer& observer,
    config::SyncPermissionsMode permissions
) {
    std::vector<LocalMoveCandidate> moves;
    for (std::size_t index = 0; index < plan.move_count(); ++index) {
        const auto& item = plan.move(index);
        if (auto previous = items.find(drive_id, item.id);
            previous.has_value()) {
            moves.push_back({
                .item = item,
                .previous = std::move(previous).value(),
                .destination = plan.state_for(item.id).local_path,
            });
        }
    }
    std::unordered_map<std::string, storage::PendingMove> pending_moves;
    std::unordered_set<std::string> recovered_moves;
    for (auto pending : items.pending_moves(drive_id)) {
        recovered_moves.insert(pending.remote_id);
        pending_moves.emplace(pending.remote_id, std::move(pending));
    }

    MoveSummary summary;
    auto schedule = build_move_schedule(moves, pending_moves);
    MovePathRemapper paths;
    MoveExecutionContext context{
        .plan = plan,
        .safe_root = safe_root,
        .drive_id = drive_id,
        .items = items,
        .operations = operations,
        .observer = observer,
        .permissions = permissions,
        .moves = moves,
        .pending_moves = pending_moves,
        .schedule = schedule,
        .summary = summary,
        .paths = paths,
        .recovered_moves = recovered_moves,
    };
    for (const auto& action : schedule.actions) {
        const auto& move = moves[action.move_index];
        if (summary.blocked.contains(move.item.id)) {
            continue;
        }
        if (action.kind == MoveActionKind::stage) {
            execute_staging_action(action.move_index, context);
        } else {
            execute_install_action(action.move_index, context);
        }
    }
    return summary;
}

}  // namespace onedrive::sync::engine_detail
