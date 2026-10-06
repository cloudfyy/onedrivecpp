#include "sync/upload/orchestration.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/remote_delete.hpp"
#include "sync/upload/remote_move.hpp"

#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/util/path_security.hpp"
#include "util/typestate.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

using util::StateTransaction;
using util::TransactionState;
using util::TransactionStateFor;
using util::transition_transaction;

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

UploadSummary upload_local_changes(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const SyncList* sync_list,
    const cli::Console& console,
    bool dry_run,
    RemoteDeletionPolicy deletion_policy,
    std::size_t upload_concurrency
) {
    if (upload_concurrency == 0) {
        throw std::invalid_argument(
            "upload concurrency must be greater than zero"
        );
    }
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
        execute_new_remote_move(
            sync_root, move, *previous, graph, items
        );
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
        execute_new_remote_delete(deletion, graph, items);
        ++summary.deleted;
        console.message(
            cli::MessageKind::information,
            "local_item_deleted",
            "Deleted remote item '" + deletion.remote_path + "'."
        );
    }
    std::size_t first_file = 0;
    while (first_file < uploads.size() &&
           uploads[first_file].directory) {
        if (!upload_directory(
                uploads[first_file],
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
        ++first_file;
    }

    std::atomic_size_t next_upload{first_file};
    std::stop_source stop;
    std::mutex result_mutex;
    std::exception_ptr first_error;
    const auto previous_failure_count =
        [&](std::string_view remote_path) {
            const auto previous =
                std::as_const(previous_failure_attempts).find(
                    std::string{remote_path}
                );
            return previous == previous_failure_attempts.end() ?
                std::uint64_t{0} :
                previous->second;
        };
    const auto upload_file = [&](const UploadCandidate& upload) {
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
                return;
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
                checkpoint,
                stop.get_token()
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
            const std::scoped_lock lock{result_mutex};
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
                previous_failure_count(upload.remote_path) + 1;
            items.save_pending_upload(pending);
            const std::scoped_lock lock{result_mutex};
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
            const std::scoped_lock lock{result_mutex};
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
                previous_failure_count(upload.remote_path) + 1;
            items.save_pending_upload(pending);
            const std::scoped_lock lock{result_mutex};
            ++summary.blocked;
            console.message(
                cli::MessageKind::warning,
                "local_upload_resource_blocked",
                "Deferred upload '" + upload.remote_path + "': " +
                    error.what()
            );
        }
    };
    const auto worker = [&] {
        while (!stop.stop_requested()) {
            const auto index =
                next_upload.fetch_add(1, std::memory_order_relaxed);
            if (index >= uploads.size()) {
                return;
            }
            try {
                upload_file(uploads[index]);
            } catch (...) {
                {
                    const std::scoped_lock lock{result_mutex};
                    if (!first_error) {
                        first_error = std::current_exception();
                    }
                }
                stop.request_stop();
                return;
            }
        }
    };

    const auto file_count = uploads.size() - first_file;
    const auto worker_count = std::min(upload_concurrency, file_count);
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        stop.request_stop();
        for (auto& thread : workers) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
    return summary;
}

}  // namespace onedrive::sync::detail
