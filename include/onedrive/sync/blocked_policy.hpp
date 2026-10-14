#pragma once

#include "onedrive/storage/item_store.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace onedrive::sync {

enum class BlockedPolicyKind {
    unsupported_format,
    safety,
    conflict,
    resource,
    remote_deletion,
    unknown,
};

enum class BlockedOrigin {
    remote,
    local,
};

enum class BlockedPolicyControl {
    fixed,
    local_conflict_configuration,
    remote_deletion_configuration,
};

enum class BlockedResolution {
    source_removed_without_pending_upload,
    safe_local_path,
    current_remote_snapshot,
    operation_completed,
    deletion_allowed,
};

struct BlockedPolicyTraits {
    BlockedPolicyKind kind;
    BlockedOrigin origin;
    BlockedPolicyControl control{BlockedPolicyControl::fixed};
    bool protects_uploads{true};
    bool protects_remote_deletions{true};
    bool preserves_remote_deletion{false};
};

class BlockedItemStrategy {
public:
    virtual ~BlockedItemStrategy() = default;

    [[nodiscard]] const BlockedPolicyTraits& traits() const noexcept {
        return traits_;
    }

    [[nodiscard]] virtual std::optional<graph::RemoteItem>
    remote_retry(const storage::BlockedItem& item, bool full_refresh) const = 0;
    [[nodiscard]] virtual bool
    resolved_by(BlockedResolution resolution) const noexcept = 0;
    [[nodiscard]] virtual bool allows_recovery() const noexcept = 0;

protected:
    explicit BlockedItemStrategy(BlockedPolicyTraits traits)
        : traits_{traits} {
    }

private:
    BlockedPolicyTraits traits_;
};

// Strategies are compiled-in and immutable; records store facts, not overrides.
[[nodiscard]] const BlockedItemStrategy&
blocked_item_strategy(std::string_view reason_code) noexcept;

[[nodiscard]] std::vector<graph::RemoteItem> blocked_ancestor_context(
    const std::string& drive_id, const storage::ItemStore& items
);

[[nodiscard]] const storage::BlockedItem* blocked_recovery(
    std::span<const storage::BlockedItem> blocked,
    std::string_view remote_id,
    std::string_view remote_path
) noexcept;

} // namespace onedrive::sync
