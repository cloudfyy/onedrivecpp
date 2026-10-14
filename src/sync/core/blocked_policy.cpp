#include "onedrive/sync/blocked_policy.hpp"
#include "sync/filter/remote_path.hpp"
#include "graph/delta_changes.hpp"

#include <array>

namespace onedrive::sync {
namespace {

enum class RemoteRestriction {
    none,
    unsupported_format,
    malware,
};

class RemoteStrategy final : public BlockedItemStrategy {
public:
    RemoteStrategy(
        BlockedPolicyTraits traits,
        RemoteRestriction restriction = RemoteRestriction::none
    )
        : BlockedItemStrategy{traits},
          restriction_{restriction} {
    }

    std::optional<graph::RemoteItem> remote_retry(
        const storage::BlockedItem& item, bool full_refresh
    ) const override {
        return graph::RemoteItem{
            .id = item.remote_id,
            .name = item.name,
            .etag = item.etag,
            .ctag = item.ctag,
            .parent_id = item.parent_id,
            .remote_path = item.remote_path,
            .last_modified = item.last_modified,
            .size = item.size,
            .directory = item.directory,
            .deleted = item.deleted || full_refresh,
            .root = false,
            .malware = restriction_ == RemoteRestriction::malware,
            .content_hash = item.content_hash,
            .unsupported_format =
                restriction_ == RemoteRestriction::unsupported_format
                    ? item.reason_code.substr(
                          std::string_view{"unsupported_"}.size()
                      )
                    : "",
        };
    }

    bool resolved_by(BlockedResolution resolution) const noexcept override {
        return resolution == BlockedResolution::operation_completed;
    }

    bool allows_recovery() const noexcept override {
        return restriction_ == RemoteRestriction::none &&
               traits().kind != BlockedPolicyKind::safety;
    }

private:
    RemoteRestriction restriction_;
};

enum class LocalRevalidation {
    path,
    operation,
    deletion,
    guarded_deletion,
};

class LocalStrategy final : public BlockedItemStrategy {
public:
    LocalStrategy(BlockedPolicyTraits traits, LocalRevalidation revalidation)
        : BlockedItemStrategy{traits},
          revalidation_{revalidation} {
    }

    std::optional<graph::RemoteItem>
    remote_retry(const storage::BlockedItem&, bool) const override {
        return std::nullopt;
    }

    bool resolved_by(BlockedResolution resolution) const noexcept override {
        switch (resolution) {
        case BlockedResolution::operation_completed:
        case BlockedResolution::current_remote_snapshot:
            return true;
        case BlockedResolution::source_removed_without_pending_upload:
            return revalidation_ == LocalRevalidation::path ||
                   revalidation_ == LocalRevalidation::operation;
        case BlockedResolution::safe_local_path:
            return revalidation_ != LocalRevalidation::operation;
        case BlockedResolution::deletion_allowed:
            return revalidation_ == LocalRevalidation::guarded_deletion;
        }
        return false;
    }

    bool allows_recovery() const noexcept override {
        return traits().kind != BlockedPolicyKind::safety;
    }

private:
    LocalRevalidation revalidation_;
};

class UnknownStrategy final : public BlockedItemStrategy {
public:
    explicit UnknownStrategy(BlockedOrigin origin)
        : BlockedItemStrategy{
              {.kind = BlockedPolicyKind::unknown, .origin = origin}
          } {
    }

    std::optional<graph::RemoteItem>
    remote_retry(const storage::BlockedItem&, bool) const override {
        return std::nullopt;
    }

    bool resolved_by(BlockedResolution) const noexcept override {
        return false;
    }

    bool allows_recovery() const noexcept override {
        return false;
    }
};

const RemoteStrategy unsupported{
    {.kind = BlockedPolicyKind::unsupported_format,
     .origin = BlockedOrigin::remote,
     .preserves_remote_deletion = true},
    RemoteRestriction::unsupported_format,
};
const RemoteStrategy malware{
    {.kind = BlockedPolicyKind::safety, .origin = BlockedOrigin::remote},
    RemoteRestriction::malware,
};
const RemoteStrategy remote_safety{
    {.kind = BlockedPolicyKind::safety, .origin = BlockedOrigin::remote},
};
const RemoteStrategy remote_conflict{
    {.kind = BlockedPolicyKind::conflict, .origin = BlockedOrigin::remote},
};
const RemoteStrategy local_modification{
    {.kind = BlockedPolicyKind::conflict,
     .origin = BlockedOrigin::remote,
     .control = BlockedPolicyControl::local_conflict_configuration},
};
const LocalStrategy local_path{
    {.kind = BlockedPolicyKind::safety,
     .origin = BlockedOrigin::local,
     .protects_uploads = false},
    LocalRevalidation::path,
};
const LocalStrategy local_type{
    {.kind = BlockedPolicyKind::conflict,
     .origin = BlockedOrigin::local,
     .protects_uploads = false},
    LocalRevalidation::path,
};
const LocalStrategy local_conflict{
    {.kind = BlockedPolicyKind::conflict,
     .origin = BlockedOrigin::local,
     .protects_uploads = false},
    LocalRevalidation::operation,
};
const LocalStrategy local_operation_safety{
    {.kind = BlockedPolicyKind::safety,
     .origin = BlockedOrigin::local,
     .protects_uploads = false},
    LocalRevalidation::operation,
};
const LocalStrategy local_resource{
    {.kind = BlockedPolicyKind::resource,
     .origin = BlockedOrigin::local,
     .protects_uploads = false},
    LocalRevalidation::operation,
};
const LocalStrategy deletion_limit{
    {.kind = BlockedPolicyKind::remote_deletion,
     .origin = BlockedOrigin::local,
     .control = BlockedPolicyControl::remote_deletion_configuration,
     .protects_uploads = false,
     .protects_remote_deletions = false},
    LocalRevalidation::guarded_deletion,
};
const LocalStrategy deletion_conflict{
    {.kind = BlockedPolicyKind::remote_deletion,
     .origin = BlockedOrigin::local,
     .protects_uploads = false,
     .protects_remote_deletions = false},
    LocalRevalidation::deletion,
};
const UnknownStrategy unknown_remote{BlockedOrigin::remote};
const UnknownStrategy unknown_local{BlockedOrigin::local};

struct Rule {
    std::string_view reason;
    const BlockedItemStrategy* strategy;
};

const std::array rules{
    Rule{"malware_detected", &malware},
    Rule{"invalid_remote_path", &remote_safety},
    Rule{"local_path_conflict", &remote_safety},
    Rule{"local_modification", &local_modification},
    Rule{"blocked_by_parent", &remote_conflict},
    Rule{"pending_move_conflict", &remote_conflict},
    Rule{"cross_device_move", &remote_conflict},
    Rule{"move_dependency_blocked", &remote_conflict},
    Rule{"upload_symbolic_link", &local_path},
    Rule{"upload_type_conflict", &local_type},
    Rule{"upload_local_path_conflict", &local_operation_safety},
    Rule{"upload_local_modification", &local_conflict},
    Rule{"upload_remote_conflict", &local_conflict},
    Rule{"upload_move_conflict", &local_conflict},
    Rule{"upload_resource", &local_resource},
    Rule{"upload_local_read", &local_resource},
    Rule{"upload_local_permission", &local_resource},
    Rule{"upload_local_storage", &local_resource},
    Rule{"upload_remote_quota", &local_resource},
    Rule{"upload_delete_limit", &deletion_limit},
    Rule{"upload_delete_conflict", &deletion_conflict},
};

} // namespace

const BlockedItemStrategy&
blocked_item_strategy(std::string_view reason_code) noexcept {
    for (const auto& rule : rules) {
        if (rule.reason == reason_code) {
            return *rule.strategy;
        }
    }
    if (reason_code.starts_with("unsupported_") &&
        reason_code.size() > std::string_view{"unsupported_"}.size()) {
        return unsupported;
    }
    return reason_code.starts_with("upload_") ? unknown_local : unknown_remote;
}

const storage::BlockedItem* blocked_recovery(
    std::span<const storage::BlockedItem> blocked,
    std::string_view remote_id,
    std::string_view remote_path
) noexcept {
    for (const auto& item : blocked) {
        if (!blocked_item_strategy(item.reason_code).allows_recovery() &&
            ((!remote_id.empty() && item.remote_id == remote_id) ||
             (!remote_path.empty() &&
              (item.remote_path == remote_path ||
               (item.directory && detail::remote_path_is_descendant(
                                      remote_path, item.remote_path
                                  )))))) {
            return &item;
        }
    }
    return nullptr;
}

std::vector<graph::RemoteItem> blocked_ancestor_context(
    const std::string& drive_id, const storage::ItemStore& items
) {
    graph::client_detail::DeltaChanges context;
    for (const auto& item : items.drive_items(drive_id)) {
        if (item.drive_id == drive_id && item.directory) {
            context.upsert({
                .id = item.remote_id,
                .name = item.name,
                .etag = item.etag,
                .ctag = item.ctag,
                .parent_id = item.parent_id,
                .remote_path = item.remote_path,
                .last_modified = item.last_modified,
                .directory = true,
                .content_hash = std::nullopt,
            });
        }
    }
    for (const auto& item : items.blocked_items(drive_id)) {
        const auto& strategy = blocked_item_strategy(item.reason_code);
        if (item.drive_id == drive_id && item.directory && !item.deleted &&
            strategy.traits().kind == BlockedPolicyKind::unsupported_format) {
            if (auto ancestor = strategy.remote_retry(item, false)) {
                context.upsert(std::move(*ancestor));
            }
        }
    }
    return std::move(context).release();
}

} // namespace onedrive::sync
