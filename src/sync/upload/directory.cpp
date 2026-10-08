#include "sync/upload/directory.hpp"
#include "sync/upload/errors.hpp"

#include "onedrive/cli/console.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "util/typestate.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace onedrive::sync::detail {

using util::StateTransaction;
using util::TransactionState;
using util::transition_transaction;

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
        .content_hash = std::nullopt,
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
struct DirectoryUploadTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, DirectoryUploadPreparedState> &&
                std::same_as<Next, DirectoryUploadJournaledState>) ||
               (std::same_as<Current, DirectoryUploadJournaledState> &&
                std::same_as<Next, DirectoryUploadGraphCreatedState>) ||
               (std::same_as<Current, DirectoryUploadGraphCreatedState> &&
                std::same_as<Next, DirectoryUploadLocalCommittedState>);
    }
};

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
    const auto identity = sync_root.identity(
        pending.local_path,
        FilesystemItemKind::directory
    );
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

void recover_pending_directory(
    const SafeSyncRoot& sync_root,
    storage::PendingUpload upload,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata
) {
    auto journaled = JournaledDirectoryUpload{
        PendingDirectoryUploadPayload{std::move(upload)},
    };
    auto graph_created =
        recover_created_directory(std::move(journaled), graph);
    static_cast<void>(commit_created_directory(
        sync_root, std::move(graph_created), items, metadata
    ));
}

}  // namespace onedrive::sync::detail
