#pragma once

#include "onedrive/graph/graph_client.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace onedrive::sync::detail {

class SyncList {
public:
    [[nodiscard]] static SyncList load(const std::filesystem::path& path);

    [[nodiscard]] bool includes(
        std::string_view remote_path,
        bool directory
    ) const;
    [[nodiscard]] bool excludes(
        std::string_view remote_path,
        bool directory
    ) const;
    [[nodiscard]] const std::string& fingerprint() const noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;

private:
    struct Rule {
        std::vector<std::string> segments;
        bool exclude{false};
        bool anchored{false};
        bool directory_only{false};
    };

    [[nodiscard]] bool matches(
        const Rule& rule,
        const std::vector<std::string_view>& path_segments,
        bool directory
    ) const;
    [[nodiscard]] bool matches_exclusion(
        const std::vector<std::string_view>& path_segments,
        bool directory
    ) const;

    std::vector<Rule> rules_;
    std::string fingerprint_;
};

struct FilteredDelta {
    graph::DeltaResult delta;
    std::size_t excluded{0};
};

[[nodiscard]] FilteredDelta filter_delta(
    graph::DeltaResult delta,
    const SyncList& sync_list,
    const std::function<bool(std::string_view)>& is_tracked,
    bool replace_drive_items
);

}  // namespace onedrive::sync::detail
