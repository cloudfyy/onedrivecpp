#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace onedrive::sync::detail {

class SyncList {
public:
    [[nodiscard]] static SyncList
    load(const std::filesystem::path& path, bool include_root_files = false);
    [[nodiscard]] static SyncList configured(
        const std::optional<std::filesystem::path>& rules_path,
        bool include_root_files,
        const std::filesystem::path& sync_root,
        bool nosync_enabled,
        bool exclude_dotfiles,
        std::uint64_t maximum_file_size_bytes
    );

    [[nodiscard]] bool includes(
        std::string_view remote_path,
        bool directory,
        std::optional<std::uint64_t> size = std::nullopt
    ) const;
    [[nodiscard]] bool excludes(
        std::string_view remote_path,
        bool directory,
        std::optional<std::uint64_t> size = std::nullopt
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
        const std::vector<std::string_view>& path_segments, bool directory
    ) const;
    [[nodiscard]] bool policy_excludes(
        std::string_view remote_path,
        const std::vector<std::string_view>& path_segments,
        bool directory,
        std::optional<std::uint64_t> size
    ) const;

    std::vector<Rule> rules_;
    std::vector<std::string> nosync_directories_;
    std::string canonical_;
    std::string fingerprint_;
    bool include_root_files_{false};
    bool select_all_{false};
    bool nosync_enabled_{false};
    bool exclude_dotfiles_{false};
    std::uint64_t maximum_file_size_bytes_{0};
};

struct FilteredDelta {
    graph::DeltaResult delta;
    std::vector<std::string> snapshot_removals;
    std::vector<std::string> retained_remote_ids;
    std::size_t excluded{0};
};

[[nodiscard]] FilteredDelta filter_delta(
    graph::DeltaResult delta,
    const SyncList& sync_list,
    const std::function<bool(std::string_view)>& is_tracked,
    storage::DeltaApplyMode apply_mode
);

} // namespace onedrive::sync::detail
