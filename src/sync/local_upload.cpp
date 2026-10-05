#include "local_upload.hpp"

#include "detail/unique_file_descriptor.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/path_security.hpp"
#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "safe_sync_root.hpp"
#include "selective_sync.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
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

std::vector<storage::PendingDelete> discover_deletions(
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list
) {
    std::unordered_set<std::string> blocked_ids;
    std::unordered_set<std::string> blocked_paths;
    for (const auto& item : items.blocked_items(drive_id)) {
        blocked_ids.insert(item.remote_id);
        blocked_paths.insert(item.remote_path);
    }
    std::vector<storage::PendingDelete> candidates;
    for (const auto& item : items.drive_items(drive_id)) {
        if (item.remote_path.empty() ||
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
                       candidate.remote_path.size() >
                           parent.remote_path.size() &&
                       candidate.remote_path.starts_with(
                           parent.remote_path
                       ) &&
                       candidate.remote_path[parent.remote_path.size()] ==
                           '/';
            }
        );
        if (!covered) {
            deletions.push_back(std::move(candidate));
        }
    }
    return deletions;
}

void execute_pending_delete(
    const storage::PendingDelete& deletion,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
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
    items.commit_delete(deletion);
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
    onedrive::detail::UniqueFileDescriptor input{
        onedrive::detail::open_path_no_symlinks(source, O_RDONLY)
    };
    std::filesystem::path snapshot_path;
    onedrive::detail::UniqueFileDescriptor output;
    for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
        snapshot_path = upload_snapshot_path(source, attempt);
        try {
            output.reset(onedrive::detail::open_path_no_symlinks(
                snapshot_path,
                O_WRONLY | O_CREAT | O_EXCL,
                S_IRUSR | S_IWUSR
            ));
            break;
        } catch (const std::runtime_error&) {
            if (!std::filesystem::exists(snapshot_path)) {
                throw;
            }
        }
    }
    if (!output) {
        throw std::runtime_error(
            "cannot allocate a local upload snapshot for: " + source.string()
        );
    }
    UploadSnapshot snapshot{snapshot_path};
    std::array<std::byte, std::size_t{64} * 1024U> buffer{};
    while (true) {
        const auto count = ::read(input.get(), buffer.data(), buffer.size());
        if (count == -1) {
            throw std::runtime_error(
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
                throw std::runtime_error(
                    "cannot write local upload snapshot '" +
                    snapshot_path.string() + "': " + std::strerror(errno)
                );
            }
            written += static_cast<std::size_t>(result);
        }
    }
    if (::fsync(output.get()) == -1) {
        throw std::runtime_error(
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
    bool cleanup_suppressions
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
            throw LocalModificationConflictError(
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
                throw LocalModificationConflictError(
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

graph::RemoteItem recover_created_directory(
    const storage::PendingUpload& upload,
    graph::GraphClient& graph
) {
    require_local_directory(upload);
    try {
        return graph.create_directory(upload.remote_path);
    } catch (const graph::UploadConflictError&) {
        const auto remote = graph.item_by_path(upload.remote_path);
        if (!remote.directory) {
            throw LocalModificationConflictError(
                "remote directory recovery conflicts with '" +
                upload.remote_path + "'"
            );
        }
        return remote;
    }
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
        if (upload.directory) {
            const auto remote =
                recover_created_directory(upload, graph);
            if (remote.remote_path != upload.remote_path) {
                throw std::runtime_error(
                    "Microsoft Graph directory response does not match '" +
                    upload.remote_path + "'"
                );
            }
            auto state = uploaded_directory_state(
                remote,
                upload.local_path,
                drive_id
            );
            const auto identity =
                sync_root.identity(upload.local_path, true);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            items.commit_upload(upload, std::move(state));
            metadata.write_remote_identity(remote, upload.local_path);
            console.message(
                cli::MessageKind::information,
                "pending_upload_recovered",
                "Recovered pending directory creation '" +
                    upload.remote_path + "'."
            );
            continue;
        }
        if (!std::filesystem::is_regular_file(upload.snapshot_path) ||
            content_fingerprint(upload.snapshot_path) !=
                upload.content_fingerprint) {
            throw std::runtime_error(
                "pending upload snapshot is missing or changed: " +
                upload.snapshot_path.string()
            );
        }
        const auto remote = recover_uploaded_item(upload, graph, items);
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
        static_cast<void>(remove_no_symlinks(upload.snapshot_path));
        console.message(
            cli::MessageKind::information,
            "pending_upload_recovered",
            "Recovered pending upload '" + upload.remote_path + "'."
        );
    }
}

void recover_pending_deletes(
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console
) {
    for (const auto& deletion : items.pending_deletes(drive_id)) {
        execute_pending_delete(deletion, graph, items);
        console.message(
            cli::MessageKind::information,
            "pending_delete_recovered",
            "Recovered remote deletion '" + deletion.remote_path + "'."
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
    bool dry_run
) {
    UploadSummary summary;
    auto deletions = discover_deletions(drive_id, items, sync_list);
    summary.planned_deletions = deletions.size();
    auto uploads = discover_uploads(
        sync_root,
        drive_id,
        items,
        sync_list,
        summary.blocked,
        console,
        !dry_run
    );
    summary.planned = static_cast<std::size_t>(std::ranges::count(
        uploads,
        false,
        &UploadCandidate::directory
    ));
    summary.planned_directories =
        uploads.size() - summary.planned;
    if (dry_run) {
        return summary;
    }
    for (const auto& deletion : deletions) {
        items.save_pending_delete(deletion);
        execute_pending_delete(deletion, graph, items);
        ++summary.deleted;
        console.message(
            cli::MessageKind::information,
            "local_item_deleted",
            "Deleted remote item '" + deletion.remote_path + "'."
        );
    }
    for (const auto& upload : uploads) {
        if (upload.directory) {
            storage::PendingUpload pending{
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
                .directory = true,
            };
            items.save_pending_upload(pending);
            graph::RemoteItem remote;
            try {
                remote = graph.create_directory(upload.remote_path);
            } catch (const graph::UploadConflictError&) {
                items.remove_pending_upload(
                    drive_id,
                    upload.remote_path
                );
                throw LocalModificationConflictError(
                    "remote item conflicts with local directory '" +
                    upload.remote_path + "'"
                );
            }
            require_local_directory(pending);
            if (remote.remote_path != upload.remote_path ||
                !remote.directory) {
                throw std::runtime_error(
                    "Microsoft Graph directory response does not match '" +
                    upload.remote_path + "'"
                );
            }
            auto state = uploaded_directory_state(
                remote,
                upload.path,
                drive_id
            );
            const auto identity = sync_root.identity(upload.path, true);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            items.commit_upload(pending, std::move(state));
            metadata.write_remote_identity(remote, upload.path);
            ++summary.created_directories;
            console.message(
                cli::MessageKind::information,
                "local_directory_created",
                "Created remote directory '" + upload.remote_path + "'."
            );
            continue;
        }
        const auto baseline = capture_local_file_baseline(upload.path);
        if (!baseline.existed) {
            continue;
        }
        auto snapshot = create_upload_snapshot(upload.path, baseline);
        storage::PendingUpload pending{
            .drive_id = drive_id,
            .remote_path = upload.remote_path,
            .local_path = upload.path,
            .snapshot_path = snapshot.path(),
            .content_fingerprint = baseline.fingerprint,
            .local_size = baseline.size,
            .local_modified_ticks = baseline.modified_ticks,
            .remote_id = upload.previous ?
                std::optional{upload.previous->remote_id} :
                std::nullopt,
            .expected_etag = upload.previous ? upload.previous->etag : "",
            .upload_url = {},
            .upload_expiration = {},
            .completed_bytes = 0,
            .directory = false,
        };
        items.save_pending_upload(pending);
        pending.snapshot_path = snapshot.release();
        const graph::UploadCheckpoint checkpoint =
            [&](const graph::UploadSession& state) {
                pending.upload_url = state.upload_url;
                pending.upload_expiration = state.expiration;
                pending.completed_bytes = state.completed_bytes;
                items.save_pending_upload(pending);
            };
        const auto remote = graph.upload_file(
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
                "Microsoft Graph upload response does not match local file '" +
                upload.path.string() + "'"
            );
        }
        auto state = uploaded_state(
            remote,
            upload.path,
            baseline,
            drive_id
        );
        const auto identity = sync_root.identity(upload.path, false);
        state.local_device = identity.device;
        state.local_inode = identity.inode;
        items.commit_upload(pending, std::move(state));
        metadata.write_remote_identity(remote, upload.path);
        static_cast<void>(remove_no_symlinks(pending.snapshot_path));
        ++summary.uploaded;
        console.message(
            cli::MessageKind::information,
            "local_item_uploaded",
            "Uploaded local file '" + upload.remote_path + "'."
        );
    }
    return summary;
}

}  // namespace onedrive::sync::detail
