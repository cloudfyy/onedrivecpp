#include "sync_plan.hpp"

#include "local_filesystem.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace onedrive::sync::detail {

SyncPlan SyncPlan::build(
    graph::DeltaResult delta,
    const std::string& drive_id,
    const std::filesystem::path& sync_directory,
    bool replace_drive_items,
    std::string sync_filter_fingerprint,
    std::vector<std::string> snapshot_removals,
    const std::vector<storage::ItemState>& tracked_items,
    std::vector<storage::UploadSuppression> upload_suppressions
) {
    SyncPlan plan;
    plan.delta_ = std::move(delta);
    plan.state_delta_.drive_id = drive_id;
    plan.state_delta_.delta_link = plan.delta_.delta_link;
    plan.state_delta_.sync_filter_fingerprint =
        std::move(sync_filter_fingerprint);
    plan.state_delta_.replace_drive_items = replace_drive_items;
    plan.state_delta_.removals = std::move(snapshot_removals);
    plan.state_delta_.upload_suppressions =
        std::move(upload_suppressions);

    for (std::size_t index = 0; index < plan.delta_.changes.size(); ++index) {
        const auto& item = plan.delta_.changes[index];
        if (item.deleted) {
            plan.removals_.push_back(index);
            spdlog::trace("Remote item deleted: id='{}'", item.id);
            continue;
        }
        if (item.root) {
            spdlog::trace("Ignoring remote drive root item '{}'", item.id);
            continue;
        }
        if (item.malware) {
            plan.block(
                item,
                "malware_detected",
                "Microsoft Graph marked the remote file as malware"
            );
            continue;
        }
        spdlog::trace(
            "Remote item changed: path='{}', id='{}', eTag='{}', type={}",
            item.remote_path,
            item.id,
            item.etag,
            item.directory ? "directory" : "file"
        );
        std::filesystem::path local_path;
        try {
            local_path = local_path_for(sync_directory, item.remote_path);
        } catch (const InvalidRemotePathError& error) {
            plan.block(item, "invalid_remote_path", error.what());
            continue;
        }
        plan.state_delta_.blocked_removals.push_back(item.id);
        const auto previous = std::ranges::find(
            tracked_items,
            item.id,
            &storage::ItemState::remote_id
        );
        if (previous != tracked_items.end() &&
            previous->remote_path != item.remote_path) {
            plan.moves_.push_back(index);
        }
        if (item.directory) {
            plan.directories_.push_back(index);
        } else {
            if (item.size < 0 ||
                static_cast<std::uint64_t>(item.size) >
                    std::numeric_limits<std::uintmax_t>::max() -
                        plan.download_bytes_) {
                throw std::runtime_error(
                    "remote download size exceeds the supported range"
                );
            }
            plan.download_bytes_ += static_cast<std::uintmax_t>(item.size);
            plan.downloads_.push_back(index);
        }
        plan.state_delta_.upserts.push_back({
            .drive_id = drive_id,
            .remote_id = item.id,
            .parent_id = item.parent_id,
            .name = item.name,
            .etag = item.etag,
            .remote_path = item.remote_path,
            .local_path = std::move(local_path),
            .last_modified = item.last_modified,
            .size = item.size,
            .local_size = 0,
            .local_modified_ticks = 0,
            .directory = item.directory,
        });
    }
    std::ranges::sort(
        plan.directories_,
        {},
        [&plan](std::size_t index) {
            return std::ranges::count(
                plan.delta_.changes[index].remote_path,
                '/'
            );
        }
    );
    return plan;
}

const graph::RemoteItem& SyncPlan::directory(std::size_t index) const {
    return delta_.changes.at(directories_.at(index));
}

const graph::RemoteItem& SyncPlan::download(std::size_t index) const {
    return delta_.changes.at(downloads_.at(index));
}

const graph::RemoteItem& SyncPlan::removal(std::size_t index) const {
    return delta_.changes.at(removals_.at(index));
}

const graph::RemoteItem& SyncPlan::move(std::size_t index) const {
    return delta_.changes.at(moves_.at(index));
}

storage::ItemState& SyncPlan::state_for(const std::string& remote_id) {
    const auto iterator = std::ranges::find(
        state_delta_.upserts,
        remote_id,
        &storage::ItemState::remote_id
    );
    if (iterator == state_delta_.upserts.end()) {
        throw std::logic_error(
            "synchronization plan is missing item state for remote ID"
        );
    }
    return *iterator;
}

void SyncPlan::block(
    const graph::RemoteItem& item,
    std::string reason_code,
    std::string reason_message
) {
    std::erase_if(
        state_delta_.upserts,
        [&item](const storage::ItemState& state) {
            return state.remote_id == item.id;
        }
    );
    std::erase(state_delta_.blocked_removals, item.id);
    const auto existing = std::ranges::find(
        state_delta_.blocked_upserts,
        item.id,
        &storage::BlockedItem::remote_id
    );
    storage::BlockedItem blocked{
        .drive_id = state_delta_.drive_id,
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .remote_path = item.remote_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .directory = item.directory,
        .reason_code = std::move(reason_code),
        .reason_message = std::move(reason_message),
        .content_hash = item.content_hash,
    };
    if (existing == state_delta_.blocked_upserts.end()) {
        state_delta_.blocked_upserts.push_back(std::move(blocked));
    } else {
        *existing = std::move(blocked);
    }
}

void SyncPlan::complete_removal(const std::string& remote_id) {
    state_delta_.removals.push_back(remote_id);
    state_delta_.blocked_removals.push_back(remote_id);
}

void SyncPlan::block_removal(
    const storage::ItemState& item,
    std::string reason_code,
    std::string reason_message
) {
    std::erase(state_delta_.blocked_removals, item.remote_id);
    state_delta_.blocked_upserts.push_back({
        .drive_id = state_delta_.drive_id,
        .remote_id = item.remote_id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .remote_path = item.remote_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .directory = item.directory,
        .deleted = true,
        .reason_code = std::move(reason_code),
        .reason_message = std::move(reason_message),
        .content_hash = std::nullopt,
    });
}

const storage::BlockedItem& SyncPlan::blocked(std::size_t index) const {
    return state_delta_.blocked_upserts.at(index);
}

storage::ItemDelta SyncPlan::release_state_delta() {
    return std::move(state_delta_);
}

std::size_t SyncPlan::change_count() const noexcept {
    return delta_.changes.size();
}

std::size_t SyncPlan::directory_count() const noexcept {
    return directories_.size();
}

std::size_t SyncPlan::download_count() const noexcept {
    return downloads_.size();
}

std::size_t SyncPlan::removal_count() const noexcept {
    return removals_.size();
}

std::size_t SyncPlan::move_count() const noexcept {
    return moves_.size();
}

std::size_t SyncPlan::blocked_count() const noexcept {
    return state_delta_.blocked_upserts.size();
}

std::uintmax_t SyncPlan::download_bytes() const noexcept {
    return download_bytes_;
}

}  // namespace onedrive::sync::detail
