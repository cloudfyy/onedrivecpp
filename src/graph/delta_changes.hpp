#pragma once

#include "onedrive/graph/graph_client.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onedrive::graph::client_detail {

// Preserve first-seen order while replacing an item's complete metadata.
class DeltaChanges {
public:
    DeltaChanges() = default;

    explicit DeltaChanges(std::vector<RemoteItem> initial) {
        changes_.reserve(initial.size());
        indexes_.reserve(initial.size());
        for (auto& item : initial) {
            upsert(std::move(item));
        }
    }

    void upsert(RemoteItem item) {
        const auto [position, inserted] =
            indexes_.emplace(item.id, changes_.size());
        if (inserted) {
            changes_.push_back(std::move(item));
        } else {
            changes_[position->second] = std::move(item);
        }
    }

    [[nodiscard]] const RemoteItem* find(const std::string& id) const {
        const auto position = indexes_.find(id);
        return position == indexes_.end() ? nullptr
                                          : &changes_[position->second];
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return changes_.size();
    }

    [[nodiscard]] std::vector<RemoteItem> release() && {
        return std::move(changes_);
    }

private:
    std::vector<RemoteItem> changes_;
    std::unordered_map<std::string, std::size_t> indexes_;
};

} // namespace onedrive::graph::client_detail
