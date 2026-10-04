#include "local_upload.hpp"

#include "detail/unique_file_descriptor.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/path_security.hpp"
#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "safe_sync_root.hpp"
#include "selective_sync.hpp"

#include <spdlog/spdlog.h>

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
};

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
    const storage::ItemStore& items,
    const SyncList* sync_list,
    std::size_t& blocked,
    const cli::Console& console
) {
    std::unordered_map<std::string, storage::ItemState> tracked;
    for (auto item : items.drive_items(drive_id)) {
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
        if (!std::filesystem::is_regular_file(status) ||
            reserved_local_name(path)) {
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
             !sync_list->includes(remote_path, false))) {
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        const auto previous = tracked.find(path.lexically_normal().string());
        if (blocked_paths.contains(remote_path) ||
            (previous != tracked.end() &&
             blocked_ids.contains(previous->second.remote_id))) {
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "cannot continue local upload scan: " + error.message()
                );
            }
            continue;
        }
        if (previous != tracked.end() && previous->second.directory) {
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
        if (previous == tracked.end() ||
            !local_snapshot_matches(previous->second, path)) {
            uploads.push_back({
                .path = path,
                .remote_path = remote_path,
                .previous = previous == tracked.end() ?
                    std::nullopt :
                    std::optional{previous->second},
            });
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(
                "cannot continue local upload scan: " + error.message()
            );
        }
    }
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

graph::RemoteItem recover_uploaded_item(
    const storage::PendingUpload& upload,
    graph::GraphClient& graph
) {
    try {
        return graph.upload_file(
            upload.remote_path,
            upload.remote_id,
            upload.expected_etag,
            upload.snapshot_path
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

}  // namespace

void recover_pending_uploads(
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const cli::Console& console
) {
    for (const auto& upload : items.pending_uploads(drive_id)) {
        if (!std::filesystem::is_regular_file(upload.snapshot_path) ||
            content_fingerprint(upload.snapshot_path) !=
                upload.content_fingerprint) {
            throw std::runtime_error(
                "pending upload snapshot is missing or changed: " +
                upload.snapshot_path.string()
            );
        }
        const auto remote = recover_uploaded_item(upload, graph);
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
    auto uploads = discover_uploads(
        sync_root,
        drive_id,
        items,
        sync_list,
        summary.blocked,
        console
    );
    summary.planned = uploads.size();
    if (dry_run) {
        return summary;
    }
    for (const auto& upload : uploads) {
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
        };
        items.save_pending_upload(pending);
        pending.snapshot_path = snapshot.release();
        const auto remote = graph.upload_file(
            upload.remote_path,
            pending.remote_id,
            pending.expected_etag,
            pending.snapshot_path
        );
        if (remote.remote_path != upload.remote_path ||
            remote.size != baseline.size) {
            throw std::runtime_error(
                "Microsoft Graph upload response does not match local file '" +
                upload.path.string() + "'"
            );
        }
        items.commit_upload(pending, uploaded_state(
            remote,
            upload.path,
            baseline,
            drive_id
        ));
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
