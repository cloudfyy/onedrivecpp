#include "onedrive/sync/sync_engine.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
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

std::optional<std::string> extended_attribute(
    const std::filesystem::path& path,
    const char* name
) {
    const auto size = ::getxattr(path.c_str(), name, nullptr, 0);
    if (size == -1) {
        if (errno == ENODATA) {
            return std::nullopt;
        }
        throw std::runtime_error(
            "cannot read synchronization metadata from '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (size != 0 &&
        ::getxattr(path.c_str(), name, value.data(), value.size()) != size) {
        throw std::runtime_error(
            "cannot read synchronization metadata from '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    return value;
}

bool remote_identity_matches(
    const graph::RemoteItem& item,
    const std::filesystem::path& path
) {
    const auto saved_ticks =
        extended_attribute(path, "user.onedrive.local_modified_ticks");
    if (!saved_ticks) {
        return false;
    }
    std::int64_t ticks{};
    const auto [position, error] = std::from_chars(
        saved_ticks->data(),
        saved_ticks->data() + saved_ticks->size(),
        ticks
    );
    if (error != std::errc{} ||
        position != saved_ticks->data() + saved_ticks->size()) {
        return false;
    }
    std::error_code file_error;
    const auto size = std::filesystem::file_size(path, file_error);
    return !file_error &&
           size == static_cast<std::uintmax_t>(item.size) &&
           modified_ticks(path) == ticks &&
           extended_attribute(path, "user.onedrive.remote_id") ==
               std::optional<std::string>{item.id} &&
           extended_attribute(path, "user.onedrive.etag") ==
               std::optional<std::string>{item.etag};
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

void download_atomically(
    graph::GraphClient& graph,
    const graph::RemoteItem& item,
    const std::filesystem::path& destination
) {
    const auto temporary = temporary_path_for(destination);
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
        set_remote_identity(item, temporary);
        std::filesystem::rename(temporary, destination);
        fsync_directory(destination.parent_path());
        spdlog::debug(
            "Atomically installed '{}' ({} bytes)",
            item.remote_path,
            downloaded_size
        );
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
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
            const auto sync_root =
                std::filesystem::weakly_canonical(config_.sync_directory);
            if (created_sync_root) {
                spdlog::debug(
                    "Created synchronization root '{}'",
                    sync_root.string()
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
                    exists && remote_identity_matches(*item, destination);
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
                    download_atomically(graph_, *item, destination);
                    ++downloaded_count;
                }
                for (auto& state : state_delta.upserts) {
                    if (state.remote_id == item->id) {
                        state.local_path = destination;
                        state.local_size = static_cast<std::int64_t>(
                            std::filesystem::file_size(destination)
                        );
                        state.local_modified_ticks = modified_ticks(destination);
                        items_.upsert(state);
                        break;
                    }
                }
                spdlog::info(
                    "Download execution completed: {} downloaded, {} reused, {} "
                    "directories prepared",
                    downloaded_count,
                    reused_count,
                    directories.size()
                );
            }
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
