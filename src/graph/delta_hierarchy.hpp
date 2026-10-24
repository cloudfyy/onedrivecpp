#pragma once

#include "onedrive/graph/graph_client.hpp"

#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace onedrive::graph::client_detail {

class DeltaHierarchyResolver {
public:
    using AncestorLookup = std::function<RemoteItem(const std::string&)>;

    DeltaHierarchyResolver(
        std::vector<RemoteItem> items,
        std::span<const RemoteItem> known_directories,
        AncestorLookup lookup,
        std::stop_token stop_token = {}
    );

    [[nodiscard]] std::vector<RemoteItem> resolve() &&;

private:
    enum class PathSource { root, parent_id, path_anchor };

    [[nodiscard]] PathSource path_source(const RemoteItem& item) const;
    [[nodiscard]] const RemoteItem& require_parent(const RemoteItem& item);
    void resolve_path(const std::string& id);

    std::vector<RemoteItem> items_;
    std::span<const RemoteItem> known_directories_;
    AncestorLookup lookup_;
    std::stop_token stop_token_;
    std::unordered_map<std::string, RemoteItem> hierarchy_;
    std::unordered_set<std::string> resolved_;
    std::vector<std::string> fetched_ids_;
};

} // namespace onedrive::graph::client_detail
