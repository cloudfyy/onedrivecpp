#include "onedrive/sync/sync_engine.hpp"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace onedrive::sync {
namespace {

std::filesystem::path local_path_for(
    const std::filesystem::path& sync_directory,
    const std::string& remote_path
) {
    const std::filesystem::path relative_path{remote_path};
    if (relative_path.empty() || relative_path.is_absolute()) {
        throw std::runtime_error("Microsoft Graph returned an invalid remote path");
    }
    for (const auto& component : relative_path) {
        if (component == "." || component == "..") {
            throw std::runtime_error(
                "Microsoft Graph returned an unsafe remote path"
            );
        }
    }
    return sync_directory / relative_path;
}

std::int64_t modified_ticks(const std::filesystem::path& path) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::filesystem::last_write_time(path).time_since_epoch()
    ).count();
}

bool local_snapshot_matches(
    const storage::ItemState& state,
    const std::filesystem::path& path
) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > static_cast<std::uintmax_t>(
                            std::numeric_limits<std::int64_t>::max()
                        )) {
        return false;
    }
    return static_cast<std::int64_t>(size) == state.local_size &&
           modified_ticks(path) == state.local_modified_ticks;
}

void set_remote_identity(
    const graph::RemoteItem& item,
    const std::filesystem::path& path
) {
    if (::setxattr(
            path.c_str(),
            "user.onedrive.remote_id",
            item.id.data(),
            item.id.size(),
            0
        ) == -1 ||
        ::setxattr(
            path.c_str(),
            "user.onedrive.etag",
            item.etag.data(),
            item.etag.size(),
            0
        ) == -1) {
        throw std::runtime_error(
            "cannot write synchronization metadata to '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    const std::string ticks = std::to_string(modified_ticks(path));
    if (::setxattr(
            path.c_str(),
            "user.onedrive.local_modified_ticks",
            ticks.data(),
            ticks.size(),
            0
        ) == -1) {
        throw std::runtime_error(
            "cannot write synchronization metadata to '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
}

bool xattr_unavailable(int error) {
    return error == ENOTSUP || error == EOPNOTSUPP || error == EPERM ||
           error == EACCES || error == ENODATA;
}

bool probe_xattr_support(const std::filesystem::path& root) {
    const auto probe = root / std::format(
                                  ".onedrive-cpp-xattr-probe-{}",
                                  ::getpid()
                              );
    const int descriptor = ::open(
        probe.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot create filesystem capability probe in '" + root.string() +
            "': " + std::strerror(errno)
        );
    }
    if (::close(descriptor) == -1) {
        const int error = errno;
        std::error_code cleanup_error;
        std::filesystem::remove(probe, cleanup_error);
        if (cleanup_error) {
            spdlog::warn(
                "Could not remove filesystem capability probe '{}': {}",
                probe.string(),
                cleanup_error.message()
            );
        }
        throw std::runtime_error(
            "cannot close filesystem capability probe: " +
            std::string{std::strerror(error)}
        );
    }

    constexpr std::string_view expected{"supported"};
    if (::setxattr(
            probe.c_str(),
            "user.onedrive.probe",
            expected.data(),
            expected.size(),
            0
        ) == -1) {
        const int error = errno;
        std::error_code cleanup_error;
        std::filesystem::remove(probe, cleanup_error);
        if (cleanup_error) {
            spdlog::warn(
                "Could not remove filesystem capability probe '{}': {}",
                probe.string(),
                cleanup_error.message()
            );
        }
        if (xattr_unavailable(error)) {
            spdlog::debug(
                "User xattr probe is unavailable for '{}': {}",
                root.string(),
                std::strerror(error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot probe user extended attributes in '" + root.string() +
            "': " + std::strerror(error)
        );
    }

    std::array<char, 32> value{};
    const auto size = ::getxattr(
        probe.c_str(),
        "user.onedrive.probe",
        value.data(),
        value.size()
    );
    const int read_error = size == -1 ? errno : 0;
    const bool valid =
        size == static_cast<ssize_t>(expected.size()) &&
        std::string_view{value.data(), static_cast<std::size_t>(size)} == expected;
    const int remove_result =
        ::removexattr(probe.c_str(), "user.onedrive.probe");
    const int remove_error = remove_result == -1 ? errno : 0;
    std::error_code cleanup_error;
    const bool probe_removed = std::filesystem::remove(probe, cleanup_error);
    if (cleanup_error || !probe_removed) {
        throw std::runtime_error(
            "cannot remove filesystem capability probe '" + probe.string() +
            "': " +
            (cleanup_error ? cleanup_error.message() : "file was not present")
        );
    }
    if (read_error != 0) {
        if (xattr_unavailable(read_error)) {
            spdlog::debug(
                "User xattr probe could not read metadata in '{}': {}",
                root.string(),
                std::strerror(read_error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot read filesystem user extended attribute probe in '" +
            root.string() + "': " + std::strerror(read_error)
        );
    }
    if (remove_error != 0) {
        if (xattr_unavailable(remove_error)) {
            spdlog::debug(
                "User xattr probe could not remove metadata in '{}': {}",
                root.string(),
                std::strerror(remove_error)
            );
            return false;
        }
        throw std::runtime_error(
            "cannot remove filesystem user extended attribute probe in '" +
            root.string() + "': " + std::strerror(remove_error)
        );
    }
    if (!valid) {
        spdlog::debug(
            "User xattr probe did not preserve metadata in '{}'",
            root.string()
        );
        return false;
    }
    spdlog::debug(
        "User xattr probe succeeded for '{}'",
        root.string()
    );
    return true;
}

std::string content_fingerprint(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open file for recovery fingerprint: " + path.string()
        );
    }
    struct DigestContextDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept {
            EVP_MD_CTX_free(context);
        }
    };
    const std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{
        EVP_MD_CTX_new()
    };
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize SHA-256 recovery fingerprint");
    }
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count != 0 &&
            EVP_DigestUpdate(
                context.get(),
                buffer.data(),
                static_cast<std::size_t>(count)
            ) != 1) {
            throw std::runtime_error("cannot update SHA-256 recovery fingerprint");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "cannot read file for recovery fingerprint: " + path.string()
        );
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(
            context.get(),
            digest.data(),
            &digest_size
        ) != 1) {
        throw std::runtime_error("cannot finalize SHA-256 recovery fingerprint");
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(digest_size) * 2);
    constexpr std::string_view hex{"0123456789abcdef"};
    for (unsigned int index = 0; index < digest_size; ++index) {
        result.push_back(hex[digest[index] >> 4U]);
        result.push_back(hex[digest[index] & 0x0FU]);
    }
    return result;
}

std::filesystem::path temporary_path_for(
    const std::filesystem::path& destination
) {
    static std::uint64_t sequence = 0;
    return destination.parent_path() /
           std::format(
               ".{}.onedrive-partial-{}-{}",
               destination.filename().string(),
               ::getpid(),
               ++sequence
           );
}

void fsync_directory(const std::filesystem::path& directory) {
    const int descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open download directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
    if (::fsync(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot flush download directory '" + directory.string() + "': " +
            message
        );
    }
    if (::close(descriptor) == -1) {
        throw std::runtime_error(
            "cannot close download directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
}

void ensure_directory_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& directory
) {
    const auto relative = directory.lexically_relative(root);
    if (relative.empty() && directory != root) {
        throw std::runtime_error(
            "local synchronization path escapes the configured directory"
        );
    }

    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current);
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "local synchronization path contains a symbolic link: " +
                current.string()
            );
        }
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                throw std::runtime_error(
                    "local path conflicts with remote directory: " +
                    current.string()
                );
            }
            continue;
        }
        std::filesystem::create_directory(current);
        spdlog::trace("Created local directory '{}'", current.string());
    }
}

storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    bool use_xattrs
) {
    const auto temporary = temporary_path_for(destination);
    bool journaled = false;
    spdlog::trace(
        "Downloading '{}' through a same-directory temporary file",
        item.remote_path
    );
    try {
        graph.download_file(item.id, temporary);
        const auto downloaded_size = std::filesystem::file_size(temporary);
        if (downloaded_size != static_cast<std::uintmax_t>(item.size)) {
            throw std::runtime_error(
                std::format(
                    "downloaded size mismatch for '{}': expected {}, received {}",
                    item.remote_path,
                    item.size,
                    downloaded_size
                )
            );
        }
        const std::string fingerprint = content_fingerprint(temporary);
        if (use_xattrs) {
            set_remote_identity(item, temporary);
        }
        state.local_path = destination;
        items.save_pending_download({
            .item = state,
            .temporary_path = temporary,
            .content_fingerprint = fingerprint,
        });
        journaled = true;
        std::filesystem::rename(temporary, destination);
        fsync_directory(destination.parent_path());
        state.local_path = destination;
        state.local_size = static_cast<std::int64_t>(downloaded_size);
        state.local_modified_ticks = modified_ticks(destination);
        items.upsert(state);
        items.remove_pending_download(state.drive_id, state.remote_id);
        spdlog::debug(
            "Atomically installed '{}' ({} bytes)",
            item.remote_path,
            downloaded_size
        );
        return state;
    } catch (const std::exception& error) {
        if (!journaled) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            if (cleanup_error) {
                spdlog::warn(
                    "Could not remove incomplete download '{}': {}",
                    temporary.string(),
                    cleanup_error.message()
                );
            }
        }
        spdlog::warn(
            "Download installation failed for '{}'; {}: {}",
            item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created",
            error.what()
        );
        throw;
    } catch (...) {
        if (!journaled) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            if (cleanup_error) {
                spdlog::warn(
                    "Could not remove incomplete download '{}': {}",
                    temporary.string(),
                    cleanup_error.message()
                );
            }
        }
        spdlog::warn(
            "Download installation failed for '{}'; {} due to an unknown error",
            item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created"
        );
        throw;
    }
}

bool recovery_file_matches(
    const std::filesystem::path& path,
    const storage::PendingDownload& download
) {
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(path, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect recovery file '" + path.string() + "': " +
            error.message()
        );
    }
    if (!regular) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read recovery file size for '" + path.string() + "': " +
            error.message()
        );
    }
    return size == static_cast<std::uintmax_t>(download.item.size) &&
           content_fingerprint(path) == download.content_fingerprint;
}

void validate_pending_download(
    const storage::PendingDownload& download,
    const std::filesystem::path& sync_root
) {
    const auto destination = download.item.local_path.lexically_normal();
    const auto temporary = download.temporary_path.lexically_normal();
    const auto destination_relative = destination.lexically_relative(sync_root);
    const auto temporary_relative = temporary.lexically_relative(sync_root);
    const auto outside_root = [](const std::filesystem::path& relative) {
        return relative.empty() || relative.is_absolute() ||
               *relative.begin() == "..";
    };
    if (outside_root(destination_relative)) {
        throw std::runtime_error(
            "pending download destination escapes the synchronization root: " +
            destination.string()
        );
    }
    if (outside_root(temporary_relative) ||
        temporary.parent_path() != destination.parent_path()) {
        throw std::runtime_error(
            "pending download temporary path is outside the destination "
            "directory: " + temporary.string()
        );
    }
    const bool valid_fingerprint =
        download.content_fingerprint.size() == 64 &&
        std::ranges::all_of(
            download.content_fingerprint,
            [](char value) {
                return (value >= '0' && value <= '9') ||
                       (value >= 'a' && value <= 'f');
            }
        );
    if (download.item.directory || download.item.size < 0 ||
        !valid_fingerprint) {
        throw std::runtime_error(
            "pending download journal contains invalid metadata for '" +
            download.item.remote_path + "'"
        );
    }
}

void recover_pending_downloads(
    storage::ItemStore& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    bool use_xattrs
) {
    const auto pending = items.pending_downloads(drive_id);
    if (pending.empty()) {
        return;
    }
    spdlog::info(
        "Recovering {} pending downloads for drive '{}'",
        pending.size(),
        drive_id
    );
    for (auto download : pending) {
        validate_pending_download(download, sync_root);
        const auto destination = download.item.local_path;
        spdlog::debug(
            "Recovering pending download '{}' to '{}'",
            download.item.remote_path,
            destination.string()
        );
        ensure_directory_tree(sync_root, destination.parent_path());
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(destination)
            ) ||
            std::filesystem::is_symlink(
                std::filesystem::symlink_status(download.temporary_path)
            )) {
            throw std::runtime_error(
                "pending download path contains a symbolic link"
            );
        }

        const bool temporary_exists =
            std::filesystem::exists(download.temporary_path);
        const bool destination_exists = std::filesystem::exists(destination);
        if (destination_exists &&
            recovery_file_matches(destination, download)) {
            if (temporary_exists) {
                if (!recovery_file_matches(download.temporary_path, download)) {
                    throw std::runtime_error(
                        "cannot remove recovery temporary file because it does "
                        "not match the journal: " +
                        download.temporary_path.string()
                    );
                }
                std::filesystem::remove(download.temporary_path);
                spdlog::debug(
                    "Removed obsolete recovery temporary file '{}'",
                    download.temporary_path.string()
                );
            }
            spdlog::debug(
                "Accepted already installed recovery destination '{}'",
                destination.string()
            );
        } else if (!destination_exists && temporary_exists &&
                   recovery_file_matches(download.temporary_path, download)) {
            std::filesystem::rename(download.temporary_path, destination);
            fsync_directory(destination.parent_path());
            spdlog::debug(
                "Promoted recovery temporary file to '{}'",
                destination.string()
            );
        } else {
            throw std::runtime_error(
                "cannot safely recover pending download '" +
                download.item.remote_path +
                "': neither destination nor temporary file matches the journal"
            );
        }

        if (use_xattrs) {
            graph::RemoteItem item{};
            item.id = download.item.remote_id;
            item.name = download.item.name;
            item.etag = download.item.etag;
            set_remote_identity(item, destination);
        }
        download.item.local_size = static_cast<std::int64_t>(
            std::filesystem::file_size(destination)
        );
        download.item.local_modified_ticks = modified_ticks(destination);
        items.upsert(download.item);
        items.remove_pending_download(drive_id, download.item.remote_id);
        spdlog::info(
            "Recovered pending download '{}'",
            download.item.remote_path
        );
    }
}

}  // namespace

SyncEngine::SyncEngine(
    const config::Config& config,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    metrics::Metrics& metrics
)
    : config_{config}, graph_{graph}, items_{items}, metrics_{metrics} {}

int SyncEngine::synchronize() const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result = [this, started_at](bool success) {
        metrics_.record_sync_run(success, std::chrono::steady_clock::now() - started_at);
    };

    try {
        std::filesystem::path sync_root = config_.sync_directory;
        bool use_xattrs = false;
        if (config_.dry_run) {
            std::cout << "Dry run configuration:\n"
                      << "  sync directory:  " << config_.sync_directory << '\n'
                      << "  state directory: " << config_.state_directory << '\n'
                      << "  drive id:        " << config_.drive_id << '\n'
                      << "  throttle retries: "
                      << config_.graph_maximum_throttle_retries << '\n'
                      << "  throttle delay:   "
                      << config_.graph_initial_throttle_delay.count() << '-'
                      << config_.graph_maximum_throttle_delay.count()
                      << " seconds\n"
                      << "  tracked items:   " << items_.size() << '\n';
            const auto pending = items_.pending_downloads(config_.drive_id);
            if (!pending.empty()) {
                spdlog::info(
                    "Dry run found {} pending downloads; recovery is deferred",
                    pending.size()
                );
            }
        } else {
            if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(config_.sync_directory)
                )) {
                throw std::runtime_error(
                    "configured synchronization directory is a symbolic link: " +
                    config_.sync_directory.string()
                );
            }
            const bool created_sync_root =
                std::filesystem::create_directories(config_.sync_directory);
            sync_root =
                std::filesystem::weakly_canonical(config_.sync_directory);
            if (created_sync_root) {
                spdlog::debug(
                    "Created synchronization root '{}'",
                    sync_root.string()
                );
            }

            if (config_.filesystem_metadata !=
                config::FilesystemMetadataMode::database) {
                use_xattrs = probe_xattr_support(sync_root);
            }
            if (config_.filesystem_metadata ==
                    config::FilesystemMetadataMode::xattr &&
                !use_xattrs) {
                throw std::runtime_error(
                    "filesystem_metadata=xattr requires user extended attribute "
                    "support"
                );
            }
            spdlog::info(
                "Filesystem metadata strategy for '{}': {}",
                sync_root.string(),
                use_xattrs ? "database journal with xattr hints" :
                             "database journal"
            );
            recover_pending_downloads(
                items_,
                sync_root,
                config_.drive_id,
                use_xattrs
            );
        }

        const auto previous_delta_link = items_.delta_link(config_.drive_id);
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_.drive_id,
            items_.size(),
            previous_delta_link ? "present" : "absent"
        );
        const auto delta = graph_.list_delta(previous_delta_link);
        storage::ItemDelta state_delta{
            .drive_id = config_.drive_id,
            .upserts = {},
            .removals = {},
            .delta_link = delta.delta_link,
        };
        std::vector<const graph::RemoteItem*> directories;
        std::vector<const graph::RemoteItem*> downloads;
        std::uintmax_t download_bytes = 0;
        for (const auto& item : delta.changes) {
            if (item.deleted) {
                state_delta.removals.push_back(item.id);
                spdlog::trace("Remote item deleted: id='{}'", item.id);
                continue;
            }
            if (item.root) {
                spdlog::trace("Ignoring remote drive root item '{}'", item.id);
                continue;
            }
            spdlog::trace(
                "Remote item changed: path='{}', id='{}', eTag='{}', type={}",
                item.remote_path,
                item.id,
                item.etag,
                item.directory ? "directory" : "file"
            );
            if (item.directory) {
                directories.push_back(&item);
            } else {
                if (item.size < 0 ||
                    static_cast<std::uint64_t>(item.size) >
                        std::numeric_limits<std::uintmax_t>::max() -
                            download_bytes) {
                    throw std::runtime_error(
                        "remote download size exceeds the supported range"
                    );
                }
                download_bytes += static_cast<std::uintmax_t>(item.size);
                downloads.push_back(&item);
            }
            state_delta.upserts.push_back({
                .drive_id = config_.drive_id,
                .remote_id = item.id,
                .parent_id = item.parent_id,
                .name = item.name,
                .etag = item.etag,
                .remote_path = item.remote_path,
                .local_path =
                    local_path_for(config_.sync_directory, item.remote_path),
                .last_modified = item.last_modified,
                .size = item.size,
                .local_size = 0,
                .local_modified_ticks = 0,
                .directory = item.directory,
            });
        }

        std::cout << "Remote delta contains " << delta.changes.size()
                  << " changes (" << state_delta.upserts.size() << " upserts, "
                  << state_delta.removals.size() << " removals).\n";
        spdlog::info(
            "Remote delta prepared for drive '{}': {} upserts, {} removals",
            config_.drive_id,
            state_delta.upserts.size(),
            state_delta.removals.size()
        );
        spdlog::info(
            "Synchronization plan for drive '{}': {} directories, {} downloads, "
            "{} bytes, {} deferred local removals",
            config_.drive_id,
            directories.size(),
            downloads.size(),
            download_bytes,
            state_delta.removals.size()
        );
        if (!state_delta.removals.empty()) {
            spdlog::warn(
                "{} remote deletions will not remove local files in this release",
                state_delta.removals.size()
            );
        }
        std::cout << "Synchronization plan:\n"
                  << "  create directories: " << directories.size() << '\n'
                  << "  download files:     " << downloads.size() << '\n'
                  << "  download bytes:     " << download_bytes << '\n'
                  << "  local removals:     0\n";
        if (config_.dry_run) {
            spdlog::debug(
                "Dry run left synchronization state unchanged for drive '{}'",
                config_.drive_id
            );
        } else {
            const auto space = std::filesystem::space(
                std::filesystem::exists(config_.sync_directory) ?
                    config_.sync_directory :
                    config_.sync_directory.parent_path()
            );
            spdlog::debug(
                "Download capacity check: {} bytes required, {} bytes available",
                download_bytes,
                space.available
            );
            if (download_bytes > space.available) {
                throw std::runtime_error(
                    std::format(
                        "synchronization requires {} bytes, but only {} bytes are "
                        "available",
                        download_bytes,
                        space.available
                    )
                );
            }

            std::ranges::sort(
                directories,
                {},
                [](const graph::RemoteItem* item) {
                    return std::ranges::count(item->remote_path, '/');
                }
            );
            for (const auto* item : directories) {
                const auto destination =
                    local_path_for(sync_root, item->remote_path);
                ensure_directory_tree(sync_root, destination);
            }
            std::size_t downloaded_count = 0;
            std::size_t reused_count = 0;
            for (const auto* item : downloads) {
                const auto destination =
                    local_path_for(sync_root, item->remote_path);
                ensure_directory_tree(sync_root, destination.parent_path());
                if (std::filesystem::is_symlink(
                        std::filesystem::symlink_status(destination)
                    )) {
                    throw std::runtime_error(
                        "local file path is a symbolic link: " +
                        destination.string()
                    );
                }
                const auto* previous = items_.find(config_.drive_id, item->id);
                const bool exists = std::filesystem::exists(destination);
                const bool current_remote_file =
                    exists && previous != nullptr &&
                    previous->etag == item->etag &&
                    local_snapshot_matches(*previous, destination);
                if (exists && !current_remote_file &&
                    (previous == nullptr ||
                     !local_snapshot_matches(*previous, destination))) {
                    spdlog::warn(
                        "Refusing to overwrite locally modified file '{}'",
                        destination.string()
                    );
                    throw std::runtime_error(
                        "local modification conflict: " + destination.string()
                    );
                }
                if (current_remote_file) {
                    spdlog::debug(
                        "Reusing completed download for '{}'",
                        item->remote_path
                    );
                    ++reused_count;
                } else {
                    spdlog::info(
                        "Downloading '{}' ({} bytes)",
                        item->remote_path,
                        item->size
                    );
                    for (auto& state : state_delta.upserts) {
                        if (state.remote_id == item->id) {
                            state = download_atomically(
                                graph_,
                                items_,
                                *item,
                                state,
                                destination,
                                use_xattrs
                            );
                            break;
                        }
                    }
                    ++downloaded_count;
                }
                for (auto& state : state_delta.upserts) {
                    if (state.remote_id == item->id) {
                        state.local_path = destination;
                        if (current_remote_file) {
                            state.local_size = static_cast<std::int64_t>(
                                std::filesystem::file_size(destination)
                            );
                            state.local_modified_ticks = modified_ticks(destination);
                        }
                        break;
                    }
                }
            }
            spdlog::info(
                "Download execution completed: {} downloaded, {} reused, {} "
                "directories prepared",
                downloaded_count,
                reused_count,
                directories.size()
            );
            spdlog::debug(
                "Persisting remote delta for drive '{}'",
                config_.drive_id
            );
            items_.apply_delta(std::move(state_delta));
        }
        record_result(true);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        if (config_.dry_run) {
            spdlog::info(
                "Synchronization dry run completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
        } else {
            spdlog::info(
                "Synchronization state update completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
        }
        return 0;
    } catch (...) {
        record_result(false);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        throw;
    }
}

}  // namespace onedrive::sync
