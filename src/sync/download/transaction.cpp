#include "sync/download/transaction.hpp"

#include "sync/download/integrity.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/safe_backup.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <limits>
#include <stdexcept>
#include <variant>

namespace onedrive::sync::detail {
namespace {

struct DownloadJournaledState final : DownloadTransactionState {};
using JournaledDownload = DownloadTransaction<DownloadJournaledState>;
using ActiveDownload = std::variant<PreparedDownload, JournaledDownload>;

template <DownloadState Next, DownloadState Current>
DownloadTransaction<Next>
transition_download(DownloadTransaction<Current>&& download) noexcept {
    return {
        .item = std::move(download.item),
        .state = std::move(download.state),
        .destination = std::move(download.destination),
        .temporary_path = std::move(download.temporary_path),
        .content_fingerprint = std::move(download.content_fingerprint),
        .downloaded_size = download.downloaded_size,
        .destination_baseline = std::move(download.destination_baseline),
        .space_reservation = std::move(download.space_reservation),
    };
}

static_assert(std::movable<JournaledDownload>);
static_assert(!std::copyable<JournaledDownload>);
static_assert(std::is_nothrow_move_constructible_v<JournaledDownload>);
static_assert(!std::same_as<PreparedDownload, JournaledDownload>);

const std::string& remote_path(const ActiveDownload& download) {
    return std::visit(
        [](const auto& active) -> const std::string& {
            return active.item.remote_path;
        },
        download
    );
}

template <DownloadState State>
void discard_download_file(
    const DownloadTransaction<State>& download
) noexcept {
    if (download.temporary_path.empty()) {
        return;
    }
    try {
        remove_no_symlinks(download.temporary_path);
    } catch (const std::exception& error) {
        spdlog::warn(
            "Could not remove incomplete download '{}': {}",
            download.temporary_path.string(),
            error.what()
        );
    }
}

} // namespace

PreparedDownload prepare_download(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    LocalFileBaseline destination_baseline,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    std::stop_token stop_token,
    const graph::DownloadProgress& progress
) {
    if (item.size < 0) {
        throw std::invalid_argument(
            "cannot download an item with a negative size"
        );
    }
    auto temporary = temporary_path_for(destination);
    std::uint64_t completed_bytes = 0;
    const auto saved =
        items.partial_download(state.drive_id, item.id);
    if (saved.has_value()) {
        const auto& partial = saved.value();
        const auto normalized_temporary =
            partial.temporary_path.lexically_normal();
        const auto normalized_destination = destination.lexically_normal();
        if (!is_temporary_path_for(
                normalized_destination,
                normalized_temporary
            )) {
            throw std::runtime_error(
                "partial download path is outside the destination directory: " +
                partial.temporary_path.string()
            );
        }

        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(
            partial.temporary_path,
            status_error
        );
        const bool missing =
            status_error == std::errc::no_such_file_or_directory ||
            (!status_error && !std::filesystem::exists(status));
        if (status_error && !missing) {
            throw std::runtime_error(
                "cannot inspect partial download '" +
                partial.temporary_path.string() + "': " +
                status_error.message()
            );
        }
        const bool metadata_matches =
            item.validate_content &&
            partial.item.etag == item.etag &&
            partial.item.size == item.size &&
            partial.item.local_path.lexically_normal() ==
                normalized_destination &&
            partial.completed_bytes <=
                static_cast<std::uint64_t>(item.size);
        if (!missing && std::filesystem::is_regular_file(status) &&
            metadata_matches) {
            const auto actual_size =
                std::filesystem::file_size(partial.temporary_path);
            if (actual_size >= partial.completed_bytes) {
                if (actual_size > partial.completed_bytes) {
                    std::filesystem::resize_file(
                        partial.temporary_path,
                        partial.completed_bytes
                    );
                }
                temporary = partial.temporary_path;
                completed_bytes = partial.completed_bytes;
                spdlog::info(
                    "Resuming partial download '{}' at byte {}",
                    item.remote_path,
                    completed_bytes
                );
            }
        }
        if (completed_bytes == 0) {
            remove_no_symlinks(partial.temporary_path);
            items.remove_partial_download(state.drive_id, item.id);
        }
    }

    const auto initial_reservation =
        item.validate_content ?
            static_cast<std::uintmax_t>(item.size) - completed_bytes :
            std::uintmax_t{0};
    auto space_reservation = space.acquire(initial_reservation);
    std::optional<StreamingDownloadHasher> streamed_hasher;
    if (completed_bytes == 0) {
        streamed_hasher.emplace();
    }
    state.local_path = destination;
    items.save_partial_download({
        .item = state,
        .temporary_path = temporary,
        .completed_bytes = completed_bytes,
    });
    spdlog::trace(
        "Downloading '{}' through a same-directory temporary file at byte {}",
        item.remote_path,
        completed_bytes
    );
    std::uintmax_t relaxed_accounted_bytes = 0;
    const auto account_relaxed_bytes = [&](std::uintmax_t observed_bytes) {
        if (item.validate_content) {
            return;
        }
        if (observed_bytes < relaxed_accounted_bytes) {
            relaxed_accounted_bytes = 0;
        }
        const auto additional_bytes =
            observed_bytes - relaxed_accounted_bytes;
        if (additional_bytes > space_reservation.remaining()) {
            space_reservation.expand(
                additional_bytes - space_reservation.remaining()
            );
        }
        space_reservation.consume(additional_bytes);
        relaxed_accounted_bytes = observed_bytes;
    };
    const graph::DownloadProgress tracked_progress =
        [&account_relaxed_bytes, &progress](
            std::uint64_t downloaded,
            std::uint64_t total
        ) {
            account_relaxed_bytes(
                static_cast<std::uintmax_t>(downloaded)
            );
            if (progress) {
                progress(downloaded, total);
            }
        };
    try {
        graph.download_file(
            item.id,
            item.etag,
            static_cast<std::uint64_t>(item.size),
            temporary,
            completed_bytes,
            std::move(stop_token),
            tracked_progress,
            [&](std::uint64_t durable_bytes) {
                if (durable_bytes < completed_bytes) {
                    throw std::logic_error(
                        "download checkpoint moved backwards"
                    );
                }
                const auto newly_durable =
                    durable_bytes - completed_bytes;
                items.save_partial_download({
                    .item = state,
                    .temporary_path = temporary,
                    .completed_bytes = durable_bytes,
                });
                space_reservation.consume(newly_durable);
                completed_bytes = durable_bytes;
            },
            [&streamed_hasher](
                std::uint64_t offset,
                std::span<const std::byte> data
            ) {
                if (streamed_hasher.has_value()) {
                    streamed_hasher->update(offset, data);
                }
            }
        );
        const auto downloaded_size = std::filesystem::file_size(temporary);
        account_relaxed_bytes(downloaded_size);
        if (item.validate_content &&
            downloaded_size != static_cast<std::uintmax_t>(item.size)) {
            throw std::runtime_error(
                std::format(
                    "downloaded size mismatch for '{}': expected {}, received {}",
                    item.remote_path,
                    item.size,
                    downloaded_size
                )
            );
        }
        auto streamed_hashes =
            streamed_hasher.has_value() ?
                streamed_hasher->finish(
                    static_cast<std::uint64_t>(downloaded_size)
                ) :
                std::nullopt;
        DownloadHashes hashes;
        if (streamed_hashes.has_value()) {
            hashes = std::move(streamed_hashes.value());
        } else {
            hashes.sha256 = content_fingerprint(temporary);
            if (item.content_hash.has_value() &&
                item.content_hash->algorithm ==
                    util::FileHashAlgorithm::quick_xor) {
                hashes.quick_xor = quick_xor_hash(temporary);
            }
        }
        const std::string& fingerprint = hashes.sha256;
        if (item.validate_content) {
            verify_download_integrity(item, hashes);
        } else {
            if (downloaded_size >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::int64_t>::max()
                )) {
                throw std::runtime_error(
                    "downloaded file is too large to track: " +
                    temporary.string()
                );
            }
            spdlog::warn(
                "Skipping remote size and hash validation for '{}' because "
                "relaxed download validation is enabled",
                item.remote_path
            );
            state.size = static_cast<std::int64_t>(downloaded_size);
        }
        apply_remote_modified_time(temporary, item.last_modified);
        metadata.write_remote_identity(item, temporary);
        fsync_file(temporary);
        return {
            .item = item,
            .state = std::move(state),
            .destination = destination,
            .temporary_path = temporary,
            .content_fingerprint = fingerprint,
            .downloaded_size = downloaded_size,
            .destination_baseline = std::move(destination_baseline),
            .space_reservation = std::move(space_reservation),
        };
    } catch (const DownloadIntegrityError& error) {
        items.remove_partial_download(state.drive_id, item.id);
        PreparedDownload incomplete;
        incomplete.temporary_path = temporary;
        discard_prepared_download(incomplete);
        spdlog::warn(
            "Download integrity verification failed for '{}': {}",
            item.remote_path,
            error.what()
        );
        throw;
    } catch (const std::exception& error) {
        if (completed_bytes == 0) {
            items.remove_partial_download(state.drive_id, item.id);
            PreparedDownload incomplete;
            incomplete.temporary_path = temporary;
            discard_prepared_download(incomplete);
        }
        spdlog::warn(
            "Download preparation failed for '{}' at byte {}: {}",
            item.remote_path,
            completed_bytes,
            error.what()
        );
        throw;
    } catch (...) {
        if (completed_bytes == 0) {
            items.remove_partial_download(state.drive_id, item.id);
            PreparedDownload incomplete;
            incomplete.temporary_path = temporary;
            discard_prepared_download(incomplete);
        }
        spdlog::warn(
            "Download preparation failed for '{}' at byte {} due to an "
            "unknown error",
            item.remote_path,
            completed_bytes
        );
        throw;
    }
}

storage::ItemState commit_download(
    storage::ItemStore& items,
    const SafeSyncRoot& sync_root,
    const FilesystemMetadata& metadata,
    PreparedDownload download,
    DownloadCommitOptions options
) {
    ActiveDownload active{
        std::in_place_type<PreparedDownload>, std::move(download)
    };
    try {
        auto baseline = std::get<PreparedDownload>(active).destination_baseline;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            auto& prepared = std::get<PreparedDownload>(active);
            if (!local_file_matches_baseline(prepared.destination, baseline)) {
                if (options.local_conflict ==
                    config::LocalConflictPolicy::block) {
                    throw LocalModificationConflictError(
                        "local file changed while downloading: " +
                        prepared.destination.string()
                    );
                }
                baseline = capture_local_file_baseline(prepared.destination);
                options.preserve_local = baseline.existed;
            }

            prepared.state.local_path = prepared.destination;
            const bool identical =
                baseline.existed &&
                baseline.fingerprint == prepared.content_fingerprint;
            std::optional<SafeBackup> backup;
            if (options.preserve_local && baseline.existed && !identical) {
                backup = preserve_safe_backup(
                    sync_root, prepared.destination, baseline
                );
                if (options.backup_created) {
                    options.backup_created(backup->path);
                }
            }

            if (!local_file_matches_baseline(prepared.destination, baseline)) {
                if (options.local_conflict ==
                    config::LocalConflictPolicy::backup) {
                    options.preserve_local = true;
                    baseline =
                        capture_local_file_baseline(prepared.destination);
                    continue;
                }
                throw LocalModificationConflictError(
                    "local file changed while downloading: " +
                    prepared.destination.string()
                );
            }

            items.save_pending_download({
                .item = prepared.state,
                .temporary_path = prepared.temporary_path,
                .content_fingerprint = prepared.content_fingerprint,
                .backup_path = backup ? backup->path : std::filesystem::path{},
                .backup_fingerprint = backup ? backup->fingerprint : "",
            });
            auto journaled =
                transition_download<DownloadJournaledState>(std::move(prepared)
                );
            active.emplace<JournaledDownload>(std::move(journaled));
            auto& installing = std::get<JournaledDownload>(active);
            items.remove_partial_download(
                installing.state.drive_id, installing.state.remote_id
            );
            if (!local_file_matches_baseline(
                    installing.destination, baseline
                )) {
                items.remove_pending_download(
                    installing.state.drive_id, installing.state.remote_id
                );
                auto retry = transition_download<DownloadPreparedState>(
                    std::move(installing)
                );
                active.emplace<PreparedDownload>(std::move(retry));
                if (options.local_conflict ==
                    config::LocalConflictPolicy::backup) {
                    options.preserve_local = true;
                    baseline = capture_local_file_baseline(
                        std::get<PreparedDownload>(active).destination
                    );
                    continue;
                }
                throw LocalModificationConflictError(
                    "local file changed while committing the download: " +
                    std::get<PreparedDownload>(active).destination.string()
                );
            }

            if (identical) {
                apply_remote_modified_time(
                    installing.destination, installing.item.last_modified
                );
                metadata.write_remote_identity(
                    installing.item, installing.destination
                );
                fsync_file(installing.destination);
                discard_download_file(installing);
                spdlog::debug(
                    "Reused content-identical local file '{}'",
                    installing.item.remote_path
                );
            } else {
                sync_root.rename(
                    installing.temporary_path, installing.destination
                );
                sync_root.fsync_directory(installing.destination.parent_path());
                spdlog::debug(
                    "Atomically installed '{}' ({} bytes)",
                    installing.item.remote_path,
                    installing.downloaded_size
                );
            }
            installing.state.local_size =
                static_cast<std::int64_t>(installing.downloaded_size);
            installing.state.local_modified_ticks =
                modified_ticks(installing.destination);
            const auto identity =
                sync_root.identity(installing.destination, false);
            installing.state.local_device = identity.device;
            installing.state.local_inode = identity.inode;
            items.upsert(installing.state);
            items.remove_pending_download(
                installing.state.drive_id, installing.state.remote_id
            );
            return installing.state;
        }
        throw LocalModificationConflictError(
            "local file changed repeatedly while creating safeBackup: " +
            std::get<PreparedDownload>(active).destination.string()
        );
    } catch (const std::exception& error) {
        const bool journaled =
            std::holds_alternative<JournaledDownload>(active);
        if (!journaled) {
            discard_prepared_download(std::get<PreparedDownload>(active));
        }
        spdlog::warn(
            "Download installation failed for '{}'; {}: {}",
            remote_path(active),
            journaled ? "recovery journal retained"
                      : "no recovery journal was created",
            error.what()
        );
        throw;
    } catch (...) {
        const bool journaled =
            std::holds_alternative<JournaledDownload>(active);
        if (!journaled) {
            discard_prepared_download(std::get<PreparedDownload>(active));
        }
        spdlog::warn(
            "Download installation failed for '{}'; {} due to an unknown error",
            remote_path(active),
            journaled ? "recovery journal retained"
                      : "no recovery journal was created"
        );
        throw;
    }
}

storage::ItemState commit_download(
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    PreparedDownload download,
    DownloadCommitOptions options
) {
    const SafeSyncRoot sync_root{download.destination.parent_path()};
    return commit_download(
        items,
        sync_root,
        metadata,
        std::move(download),
        std::move(options)
    );
}

void discard_prepared_download(const PreparedDownload& download) noexcept {
    discard_download_file(download);
}

storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    const graph::DownloadProgress& progress
) {
    return commit_download(
        items,
        metadata,
        prepare_download(
            graph,
            items,
            item,
            std::move(state),
            destination,
            capture_local_file_baseline(destination),
            metadata,
            space,
            {},
            progress
        )
    );
}

}  // namespace onedrive::sync::detail
