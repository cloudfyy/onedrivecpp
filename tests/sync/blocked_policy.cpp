#include "support/progress.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include "support/common.hpp"

#include <array>
#include <stdexcept>

namespace {

using namespace onedrive;
using namespace onedrive::sync;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

storage::BlockedItem record(std::string reason) {
    storage::BlockedItem item;
    item.drive_id = "drive";
    item.remote_id = "id";
    item.parent_id = "parent";
    item.name = "item";
    item.remote_path = "folder/item";
    item.etag = "etag";
    item.ctag = "ctag";
    item.size = 42;
    item.reason_code = std::move(reason);
    item.reason_message = "blocked";
    return item;
}

void test_remote_strategies() {
    for (const auto reason : {"unsupported_onenote", "unsupported_future"}) {
        auto item = record(reason);
        const auto& strategy = blocked_item_strategy(item.reason_code);
        const auto retry = strategy.remote_retry(item, false);
        require(
            strategy.traits().kind == BlockedPolicyKind::unsupported_format &&
                strategy.traits().control == BlockedPolicyControl::fixed &&
                strategy.traits().preserves_remote_deletion &&
                strategy.traits().protects_uploads &&
                strategy.traits().protects_remote_deletions &&
                !strategy.allows_recovery() && retry &&
                retry->id == item.remote_id && retry->etag == item.etag &&
                retry->ctag == item.ctag && retry->size == 42 &&
                !retry->deleted &&
                retry->unsupported_format == item.reason_code.substr(12) &&
                strategy.remote_retry(item, true)->deleted &&
                !strategy.resolved_by(BlockedResolution::safe_local_path) &&
                !strategy.resolved_by(BlockedResolution::deletion_allowed),
            "unsupported format policy allowed an override or lost retry "
            "classification"
        );
    }
    for (const auto reason :
         {"malware_detected",
          "invalid_remote_path",
          "local_path_conflict",
          "local_modification",
          "blocked_by_parent",
          "pending_move_conflict",
          "cross_device_move",
          "move_dependency_blocked"}) {
        auto item = record(reason);
        const auto& strategy = blocked_item_strategy(item.reason_code);
        const auto retry = strategy.remote_retry(item, false);
        require(
            retry && retry->id == item.remote_id &&
                retry->malware == (item.reason_code == "malware_detected") &&
                retry->unsupported_format.empty() &&
                strategy.traits().origin == BlockedOrigin::remote &&
                strategy.traits().protects_uploads &&
                strategy.traits().protects_remote_deletions &&
                !strategy.traits().preserves_remote_deletion &&
                strategy.allows_recovery() ==
                    (strategy.traits().kind != BlockedPolicyKind::safety) &&
                strategy.resolved_by(BlockedResolution::operation_completed),
            "remote conflict/safety strategy changed revalidation or protection"
        );
    }
    require(
        blocked_item_strategy("local_modification").traits().control ==
                BlockedPolicyControl::local_conflict_configuration &&
            blocked_item_strategy("malware_detected").traits().control ==
                BlockedPolicyControl::fixed,
        "configurable conflict policy was confused with a safety restriction"
    );
}

void test_local_strategies() {
    for (const auto reason :
         {"upload_symbolic_link",
          "upload_type_conflict",
          "upload_local_path_conflict",
          "upload_local_modification",
          "upload_remote_conflict",
          "upload_move_conflict",
          "upload_resource",
          "upload_local_read",
          "upload_local_permission",
          "upload_local_storage",
          "upload_remote_quota",
          "upload_remote_permission",
          "upload_delete_limit",
          "upload_delete_conflict",
          "upload_delete_permission"}) {
        const auto item = record(reason);
        const auto& strategy = blocked_item_strategy(item.reason_code);
        const bool deletion = item.reason_code.starts_with("upload_delete_");
        const bool path = item.reason_code == "upload_symbolic_link" ||
                          item.reason_code == "upload_type_conflict";
        require(
            strategy.traits().origin == BlockedOrigin::local &&
                !strategy.remote_retry(item, false) &&
                !strategy.remote_retry(item, true) &&
                !strategy.traits().protects_uploads &&
                strategy.traits().protects_remote_deletions == !deletion &&
                strategy.allows_recovery() ==
                    (strategy.traits().kind != BlockedPolicyKind::safety) &&
                strategy.resolved_by(BlockedResolution::safe_local_path) ==
                    (path || deletion) &&
                strategy.resolved_by(
                    BlockedResolution::source_removed_without_pending_upload
                ) == !deletion &&
                strategy.resolved_by(BlockedResolution::deletion_allowed) ==
                    (item.reason_code == "upload_delete_limit") &&
                strategy.resolved_by(
                    BlockedResolution::current_remote_snapshot
                ) &&
                strategy.resolved_by(BlockedResolution::operation_completed),
            "local strategy allowed premature clearing or a synthetic Graph "
            "retry"
        );
    }
    require(
        blocked_item_strategy("upload_delete_limit").traits().control ==
                BlockedPolicyControl::remote_deletion_configuration &&
            blocked_item_strategy("upload_delete_conflict").traits().control ==
                BlockedPolicyControl::fixed,
        "deletion approval was exposed for a non-overridable conflict"
    );
}

void test_unknown_policy_is_conservative() {
    for (const auto reason :
         {"",
          "future_reason",
          "unsupported_",
          "upload_future",
          "upload_delete_future"}) {
        const auto item = record(reason);
        const auto& strategy = blocked_item_strategy(item.reason_code);
        require(
            strategy.traits().kind == BlockedPolicyKind::unknown &&
                strategy.traits().control == BlockedPolicyControl::fixed &&
                strategy.traits().protects_uploads &&
                strategy.traits().protects_remote_deletions &&
                !strategy.allows_recovery() &&
                !strategy.remote_retry(item, false) &&
                !strategy.remote_retry(item, true),
            "unknown reason selected an unsafe default strategy"
        );
        for (const auto resolution :
             {BlockedResolution::source_removed_without_pending_upload,
              BlockedResolution::safe_local_path,
              BlockedResolution::current_remote_snapshot,
              BlockedResolution::operation_completed,
              BlockedResolution::deletion_allowed}) {
            require(
                !strategy.resolved_by(resolution),
                "unknown blocker was automatically cleared"
            );
        }
    }
}

void test_recovery_guard_matches_identity_and_subtrees() {
    auto notes = record("unsupported_onenote");
    notes.remote_path = "Notebook";
    notes.directory = true;
    const std::array blocked{notes, record("upload_remote_quota")};
    require(
        blocked_recovery(blocked, "child", "Notebook/section.one") ==
                &blocked[0] &&
            blocked_recovery(blocked, notes.remote_id, "renamed.one") ==
                &blocked[0] &&
            blocked_recovery(blocked, "sibling", "Notebook-copy/section.one") ==
                nullptr &&
            blocked_recovery(blocked, "other", "folder/item") == nullptr,
        "recovery guard missed a blocked identity/subtree or blocked an "
        "independent transfer"
    );
    notes.directory = false;
    const std::array file_blocked{notes};
    require(
        blocked_recovery(file_blocked, "", "Notebook") == &file_blocked[0] &&
            blocked_recovery(file_blocked, "", "") == nullptr &&
            blocked_recovery(file_blocked, "other", "") == nullptr &&
            blocked_recovery(file_blocked, "", "Notebook/section.one") ==
                nullptr &&
            blocked_recovery({}, notes.remote_id, notes.remote_path) == nullptr,
        "recovery guard treated empty identities or a file as a subtree"
    );
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{4};
    try {
        progress.run("remote_strategies", test_remote_strategies);
        progress.run("local_strategies", test_local_strategies);
        progress.run(
            "unknown_policy_is_conservative",
            test_unknown_policy_is_conservative
        );
        progress.run(
            "recovery_guard_matches_identity_and_subtrees",
            test_recovery_guard_matches_identity_and_subtrees
        );
        return progress.finish();
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
