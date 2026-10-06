#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace onedrive::sync::detail {

enum class SyncItemKind {
    file,
    directory,
};

struct SyncFilterPolicy {
    std::optional<std::filesystem::path> rules_path;
    std::filesystem::path sync_root;
    bool include_root_files{false};
    bool nosync_enabled{true};
    config::DotfilePolicy dotfiles{config::DotfilePolicy::include};
    std::uint64_t maximum_file_size_bytes{0};
};

class SyncList {
public:
    [[nodiscard]] static SyncList
    load(const std::filesystem::path& path, bool include_root_files = false);
    [[nodiscard]] static SyncList configured(const SyncFilterPolicy& policy);

    [[nodiscard]] bool includes(
        std::string_view remote_path,
        SyncItemKind kind,
        std::optional<std::uint64_t> size = std::nullopt
    ) const;
    [[nodiscard]] bool excludes(
        std::string_view remote_path,
        SyncItemKind kind,
        std::optional<std::uint64_t> size = std::nullopt
    ) const;
    [[nodiscard]] const std::string& fingerprint() const noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;

private:
    enum class RuleAction {
        include,
        exclude,
    };

    enum class RuleAnchor {
        any_depth,
        drive_root,
    };

    enum class RuleTarget {
        any,
        directory,
    };

    struct Rule {
        std::vector<std::string> segments;
        RuleAction action{RuleAction::include};
        RuleAnchor anchor{RuleAnchor::any_depth};
        RuleTarget target{RuleTarget::any};
    };

    [[nodiscard]] bool matches(
        const Rule& rule,
        const std::vector<std::string_view>& path_segments,
        SyncItemKind kind
    ) const;
    [[nodiscard]] bool matches_exclusion(
        const std::vector<std::string_view>& path_segments,
        SyncItemKind kind
    ) const;
    [[nodiscard]] bool policy_excludes(
        std::string_view remote_path,
        const std::vector<std::string_view>& path_segments,
        SyncItemKind kind,
        std::optional<std::uint64_t> size
    ) const;

    std::vector<Rule> rules_;
    std::vector<std::string> nosync_directories_;
    std::string canonical_;
    std::string fingerprint_;
    bool include_root_files_{false};
    bool select_all_{false};
    bool nosync_enabled_{false};
    config::DotfilePolicy dotfiles_{config::DotfilePolicy::include};
    std::uint64_t maximum_file_size_bytes_{0};
};

struct FilteredDelta {
    graph::DeltaResult delta;
    std::vector<std::string> snapshot_removals;
    std::vector<std::string> retained_remote_ids;
    std::size_t excluded{0};
};

class TrackedItemPredicate {
public:
    template <typename Callable>
        requires std::predicate<const std::remove_reference_t<Callable>&,
                                std::string_view>
    TrackedItemPredicate(Callable&& callable) noexcept
        : object_{std::addressof(callable)},
          invoke_{[](const void* object, std::string_view remote_id) {
              return std::invoke(
                  *static_cast<
                      const std::remove_reference_t<Callable>*>(object),
                  remote_id
              );
          }} {}

    [[nodiscard]] bool operator()(std::string_view remote_id) const {
        return invoke_(object_, remote_id);
    }

private:
    const void* object_;
    bool (*invoke_)(const void*, std::string_view);
};

[[nodiscard]] FilteredDelta filter_delta(
    graph::DeltaResult delta,
    const SyncList& sync_list,
    TrackedItemPredicate is_tracked,
    storage::DeltaApplyMode apply_mode
);

} // namespace onedrive::sync::detail
