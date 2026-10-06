#include "sync/core/delta_plan.hpp"
#include "sync/filter/remote_path.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace onedrive::sync::engine_detail {

graph::RemoteItem remote_item(const storage::BlockedItem& item) {
    return {
        .id = item.remote_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .parent_id = item.parent_id,
        .remote_path = item.remote_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .directory = item.directory,
        .deleted = item.deleted,
        .root = false,
        .malware = item.reason_code == "malware_detected",
        .content_hash = item.content_hash,
    };
}

void add_blocked_retries(
    graph::DeltaResult& delta,
    const std::vector<storage::BlockedItem>& blocked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }
    for (const auto& item : blocked) {
        if (!changed_ids.contains(item.remote_id)) {
            delta.changes.push_back(remote_item(item));
        }
    }
}

void add_full_refresh_deletions(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> remote_ids;
    remote_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        remote_ids.insert(item.id);
    }
    for (const auto& item : tracked) {
        if (!remote_ids.contains(item.remote_id)) {
            graph::RemoteItem deletion;
            deletion.id = item.remote_id;
            deletion.deleted = true;
            delta.changes.push_back(std::move(deletion));
        }
    }
}

void add_deleted_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }
    std::vector<std::string> deleted_directories;
    for (const auto& change : delta.changes) {
        if (!change.deleted) {
            continue;
        }
        const auto previous = std::ranges::find(
            tracked,
            change.id,
            &storage::ItemState::remote_id
        );
        if (previous != tracked.end() && previous->directory) {
            deleted_directories.push_back(previous->remote_path);
        }
    }
    for (const auto& item : tracked) {
        const bool below_deleted_directory = std::ranges::any_of(
            deleted_directories,
            [&](const std::string& directory) {
                return detail::remote_path_is_descendant(
                    item.remote_path,
                    directory
                );
            }
        );
        if (changed_ids.contains(item.remote_id) ||
            !below_deleted_directory) {
            continue;
        }
        graph::RemoteItem deletion;
        deletion.id = item.remote_id;
        deletion.deleted = true;
        delta.changes.push_back(std::move(deletion));
        changed_ids.insert(item.remote_id);
    }
}

void add_moved_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }

    std::vector<graph::RemoteItem> descendants;
    for (const auto& change : delta.changes) {
        if (change.deleted || !change.directory) {
            continue;
        }
        const auto previous = std::ranges::find(
            tracked,
            change.id,
            &storage::ItemState::remote_id
        );
        if (previous == tracked.end() ||
            previous->remote_path == change.remote_path) {
            continue;
        }
        for (const auto& item : tracked) {
            if (changed_ids.contains(item.remote_id) ||
                !detail::remote_path_is_descendant(
                    item.remote_path,
                    previous->remote_path
                )) {
                continue;
            }
            auto remote_path =
                change.remote_path +
                item.remote_path.substr(previous->remote_path.size());
            descendants.push_back({
                .id = item.remote_id,
                .name = item.name,
                .etag = item.etag,
                .ctag = item.ctag,
                .parent_id = item.parent_id,
                .remote_path = std::move(remote_path),
                .last_modified = item.last_modified,
                .size = item.size,
                .directory = item.directory,
                .content_hash = std::nullopt,
            });
            changed_ids.insert(item.remote_id);
        }
    }
    delta.changes.insert(
        delta.changes.end(),
        std::make_move_iterator(descendants.begin()),
        std::make_move_iterator(descendants.end())
    );
}

bool below_blocked_directory(
    std::string_view path,
    const std::vector<std::string>& blocked_directories
) {
    return std::ranges::any_of(
        blocked_directories,
        [path](const std::string& directory) {
            return detail::remote_path_is_descendant(path, directory);
        }
    );
}

}  // namespace onedrive::sync::engine_detail
