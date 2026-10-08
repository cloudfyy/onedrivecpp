#include "sync/upload/file.hpp"
#include "sync/upload/directory.hpp"
#include "sync/upload/errors.hpp"
#include "sync/upload/orchestration.hpp"

#include "onedrive/events/observer.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "util/typestate.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace onedrive::sync::detail {

using util::StateTransaction;
using util::TransactionState;
using util::TransactionStateFor;
using util::transition_transaction;

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
struct UploadTransactionFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return (std::same_as<Current, UploadPreparedState> &&
                std::same_as<Next, UploadJournaledState>) ||
               (std::same_as<Current, UploadJournaledState> &&
                std::same_as<Next, UploadRemoteCommittedState>);
    }
};

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
    onedrive::util::UniqueFD input;
    try {
        input = onedrive::util::open_path_no_symlinks(source, O_RDONLY);
    } catch (const std::system_error& error) {
        throw LocalUploadResourceError(
            local_resource_code(error.code(), "local_read"),
            "cannot open local upload source '" + source.string() +
                "': " + error.code().message()
        );
    }
    std::filesystem::path snapshot_path;
    onedrive::util::UniqueFD output;
    for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
        snapshot_path = upload_snapshot_path(source, attempt);
        try {
            output = onedrive::util::open_path_no_symlinks(
                snapshot_path,
                O_WRONLY | O_CREAT | O_EXCL,
                S_IRUSR | S_IWUSR
            );
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
                "': " + onedrive::util::system_error_message(errno)
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
                    snapshot_path.string() + "': " + onedrive::util::system_error_message(errno)
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
            "': " + onedrive::util::system_error_message(errno)
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
        .content_hash = std::nullopt,
        .directory = false,
    };
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

FileUploadResult execute_file_upload(
    const UploadCandidate& upload,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    std::uint64_t previous_failure_count,
    const std::stop_token& stop_token
) {
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
        const auto baseline = capture_local_file_baseline(upload.path);
        if (!baseline.existed) {
            return {FileUploadStatus::skipped, {}};
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
        const auto checkpoint = persist_upload_checkpoints(journaled, items);
        auto remote = graph.upload_file(
            upload.remote_path,
            pending.remote_id,
            pending.expected_etag,
            pending.snapshot_path,
            std::nullopt,
            checkpoint,
            stop_token
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
        const auto identity = sync_root.identity(
            upload.path,
            FilesystemItemKind::file
        );
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
        return {FileUploadStatus::uploaded, {}};
    } catch (const LocalUploadResourceError& error) {
        pending.failure_code = error.reason_code();
        pending.failure_message = error.what();
        pending.failure_attempt_count = previous_failure_count + 1;
        items.save_pending_upload(pending);
        return {FileUploadStatus::blocked, error.what()};
    } catch (const graph::UploadResourceError& error) {
        pending.failure_code = error.reason_code();
        pending.failure_message = error.what();
        ++pending.failure_attempt_count;
        items.save_pending_upload(pending);
        return {FileUploadStatus::blocked, error.what()};
    } catch (const std::system_error& error) {
        pending.failure_code = local_resource_code(error.code(), "local_read");
        pending.failure_message = error.what();
        pending.failure_attempt_count = previous_failure_count + 1;
        items.save_pending_upload(pending);
        return {FileUploadStatus::blocked, error.what()};
    }
}

void recover_pending_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const events::Observer& observer
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
                recover_pending_directory(
                    sync_root, upload, graph, items, metadata
                );
                observer.message(
                    events::MessageKind::information,
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
            const auto identity = sync_root.identity(
                upload.local_path,
                FilesystemItemKind::file
            );
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            items.commit_upload(upload, state);
            metadata.write_remote_identity(remote, upload.local_path);
            static_cast<void>(
                remove_no_symlinks(upload.snapshot_path)
            );
            observer.message(
                events::MessageKind::information,
                "pending_upload_recovered",
                "Recovered pending upload '" + upload.remote_path + "'."
            );
        } catch (const graph::UploadResourceError& error) {
            upload.failure_code = error.reason_code();
            upload.failure_message = error.what();
            ++upload.failure_attempt_count;
            items.save_pending_upload(upload);
            observer.message(
                events::MessageKind::warning,
                "pending_upload_resource_blocked",
                "Deferred pending upload '" + upload.remote_path +
                    "': " + error.what()
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
            observer.message(
                events::MessageKind::warning,
                "pending_upload_conflict_deferred",
                std::string{error.what()} +
                    "; reconciling it through the remote delta."
            );
        }
    }
}

}  // namespace onedrive::sync::detail
