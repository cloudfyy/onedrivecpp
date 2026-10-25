#include "graph/delta_hierarchy.hpp"
#include "graph/support.hpp"

#include <stdexcept>
#include <utility>

namespace onedrive::graph::client_detail {

DeltaHierarchyResolver::DeltaHierarchyResolver(
    std::vector<RemoteItem> items,
    std::span<const RemoteItem> known_directories,
    AncestorLookup lookup,
    std::stop_token stop_token
)
    : items_{std::move(items)},
      known_directories_{known_directories},
      lookup_{std::move(lookup)},
      stop_token_{std::move(stop_token)} {
}

DeltaHierarchyResolver::PathSource
DeltaHierarchyResolver::path_source(const RemoteItem& item) const {
    if (item.root) {
        return PathSource::root;
    }
    if (!hierarchy_.contains(item.parent_id) && !item.remote_path.empty()) {
        return PathSource::path_anchor;
    }
    return PathSource::parent_id;
}

const RemoteItem&
DeltaHierarchyResolver::require_parent(const RemoteItem& item) {
    auto parent = hierarchy_.find(item.parent_id);
    if (parent == hierarchy_.end()) {
        if (item.parent_id.empty()) {
            throw std::runtime_error{
                "Microsoft Graph delta path is missing a parent ID "
                "for item '" +
                item.id + "'"
            };
        }
        const auto parent_id = item.parent_id;
        auto fetched = lookup_(parent_id);
        if (fetched.id != parent_id || !fetched.directory || fetched.deleted) {
            throw std::runtime_error{
                "Microsoft Graph returned an invalid delta ancestor"
            };
        }
        fetched_ids_.push_back(parent_id);
        parent = hierarchy_.emplace(parent_id, std::move(fetched)).first;
    }
    if (parent->second.deleted || !parent->second.directory) {
        throw std::runtime_error{
            "Microsoft Graph returned an invalid delta ancestor"
        };
    }
    return parent->second;
}

void DeltaHierarchyResolver::resolve_path(const std::string& id) {
    std::vector<std::string> chain;
    std::unordered_set<std::string> visiting;
    auto current_id = id;
    while (!resolved_.contains(current_id)) {
        if (stop_token_.stop_requested()) {
            throw RequestCancelledError{
                "Microsoft Graph delta path resolution was cancelled"
            };
        }
        if (!visiting.insert(current_id).second) {
            throw std::runtime_error{
                "Microsoft Graph returned cyclic delta parent IDs"
            };
        }
        auto& current = hierarchy_.at(current_id);
        const auto source = path_source(current);
        if (source == PathSource::root) {
            current.remote_path.clear();
        }
        if (source != PathSource::parent_id) {
            resolved_.insert(current_id);
            break;
        }
        const auto& parent = require_parent(current);
        chain.push_back(current_id);
        current_id = parent.id;
    }
    for (auto position = chain.rbegin(); position != chain.rend(); ++position) {
        auto& child = hierarchy_.at(*position);
        if (child.name.empty()) {
            throw std::runtime_error{
                "Microsoft Graph delta ancestry is missing an item name"
            };
        }
        const auto& parent = hierarchy_.at(child.parent_id);
        child.remote_path = parent.remote_path.empty()
                                ? child.name
                                : parent.remote_path + '/' + child.name;
        resolved_.insert(child.id);
    }
}

std::vector<RemoteItem> DeltaHierarchyResolver::resolve() && {
    resolve_unsupported_ancestry(
        items_, known_directories_, lookup_, stop_token_
    );
    for (const auto& directory : known_directories_) {
        hierarchy_.insert_or_assign(directory.id, directory);
    }
    for (const auto& item : items_) {
        hierarchy_.insert_or_assign(item.id, item);
    }
    for (const auto& item : items_) {
        if (!item.deleted) {
            resolve_path(item.id);
        }
    }
    for (auto& item : items_) {
        if (!item.deleted) {
            item.remote_path = hierarchy_.at(item.id).remote_path;
        }
    }
    for (const auto& id : fetched_ids_) {
        items_.push_back(std::move(hierarchy_.at(id)));
    }
    mark_unsupported_descendants(items_, known_directories_);
    return std::move(items_);
}

} // namespace onedrive::graph::client_detail
