#pragma once

#include "onedrive/storage/item_store.hpp"
#include "sync/core/delta_plan.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onedrive::test::sync {

class FakeItemStore final {
public:
    void open() {
    }

    void open_read_only() {
    }

    void upsert(onedrive::storage::ItemState item) {
        const std::scoped_lock lock{mutex};
        if (fail_upsert) {
            throw std::runtime_error{"simulated item persistence failure"};
        }
        ++upsert_count;
        items.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta delta) {
        const std::scoped_lock lock{mutex};
        if (fail_apply_delta) {
            throw std::runtime_error{"simulated delta persistence failure"};
        }
        ++apply_count;
        if (delta.apply_mode == onedrive::storage::DeltaApplyMode::replace) {
            std::erase_if(items, [&delta](const auto& entry) {
                return std::ranges::none_of(
                    delta.blocked_upserts, [&entry](const auto& item) {
                        return item.remote_id == entry.first;
                    }
                );
            });
        }
        for (const auto& remote_id : delta.removals) {
            items.erase(remote_id);
        }
        for (const auto& remote_id : delta.partial_download_removals) {
            partials.erase(remote_id);
        }
        for (const auto& item : delta.upserts) {
            pending_moves_by_id.erase(item.remote_id);
            items.insert_or_assign(item.remote_id, item);
        }
        for (const auto& remote_id : delta.removals) {
            pending_moves_by_id.erase(remote_id);
        }
        for (const auto& suppression : delta.upload_suppressions) {
            upload_suppressions_by_path.insert_or_assign(
                suppression.local_path.lexically_normal().string(), suppression
            );
        }
        for (const auto& remote_id : delta.blocked_removals) {
            std::erase_if(blocked, [&remote_id](const auto& item) {
                return item.remote_id == remote_id;
            });
        }
        for (const auto& item : delta.blocked_upserts) {
            if (delta.blocked_items_saved) {
                break;
            }
            const auto existing = std::ranges::find(
                blocked,
                item.remote_id,
                &onedrive::storage::BlockedItem::remote_id
            );
            if (existing == blocked.end()) {
                blocked.push_back(item);
            } else {
                *existing = item;
            }
        }
        applied_delta = std::move(delta);
        pending_remote_delta.reset();
    }

    void save_pending_delta(onedrive::storage::PendingDelta delta) {
        const std::scoped_lock lock{mutex};
        if (fail_save_pending_delta) {
            throw std::runtime_error{
                "simulated pending delta persistence failure"
            };
        }
        pending_remote_delta = std::move(delta);
    }
    [[nodiscard]] std::optional<onedrive::storage::PendingDelta>
    pending_delta(const std::string&) const {
        const std::scoped_lock lock{mutex};
        return pending_remote_delta;
    }
    void remove_pending_delta(const std::string&) {
        const std::scoped_lock lock{mutex};
        pending_remote_delta.reset();
    }

    void save_pending_download(onedrive::storage::PendingDownload download) {
        const std::scoped_lock lock{mutex};
        pending.insert_or_assign(download.item.remote_id, std::move(download));
    }

    void
    remove_pending_download(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        pending.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingDownload> result;
        for (const auto& [remote_id, download] : pending) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_partial_download(onedrive::storage::PartialDownload download) {
        const std::scoped_lock lock{mutex};
        partials.insert_or_assign(download.item.remote_id, std::move(download));
    }

    void
    remove_partial_download(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        partials.erase(remote_id);
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(const std::string&, const std::string& remote_id) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = partials.find(remote_id);
        return iterator == partials.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::PartialDownload>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::vector<onedrive::storage::PartialDownload>
    partial_downloads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PartialDownload> result;
        result.reserve(partials.size());
        for (const auto& [remote_id, download] : partials) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_pending_upload(onedrive::storage::PendingUpload upload) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_upload_save) {
            throw std::runtime_error{
                "simulated pending upload persistence failure"
            };
        }
        if (fail_upload_checkpoint_save && !upload.upload_url.empty()) {
            throw std::runtime_error{
                "simulated upload checkpoint persistence failure"
            };
        }
        pending_uploads_by_path.insert_or_assign(
            upload.remote_path, std::move(upload)
        );
    }

    void
    remove_pending_upload(const std::string&, const std::string& remote_path) {
        const std::scoped_lock lock{mutex};
        pending_uploads_by_path.erase(remote_path);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingUpload> result;
        for (const auto& [path, upload] : pending_uploads_by_path) {
            static_cast<void>(path);
            result.push_back(upload);
        }
        return result;
    }

    void commit_upload(
        const onedrive::storage::PendingUpload& upload,
        onedrive::storage::ItemState item
    ) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_upload) {
            throw std::runtime_error{"simulated upload commit failure"};
        }
        ++upsert_count;
        items.insert_or_assign(item.remote_id, std::move(item));
        pending_uploads_by_path.erase(upload.remote_path);
    }

    void save_pending_delete(onedrive::storage::PendingDelete deletion) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_delete_save) {
            throw std::runtime_error{
                "simulated pending deletion persistence failure"
            };
        }
        pending_deletes_by_id.insert_or_assign(
            deletion.remote_id, std::move(deletion)
        );
    }

    void
    remove_pending_delete(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        pending_deletes_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDelete>
    pending_deletes(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingDelete> result;
        for (const auto& [remote_id, deletion] : pending_deletes_by_id) {
            static_cast<void>(remote_id);
            result.push_back(deletion);
        }
        return result;
    }

    void commit_delete(const onedrive::storage::PendingDelete& deletion) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_delete) {
            throw std::runtime_error{"simulated deletion commit failure"};
        }
        for (auto iterator = items.begin(); iterator != items.end();) {
            const auto& path = iterator->second.remote_path;
            const bool descendant = path.size() > deletion.remote_path.size() &&
                                    path.starts_with(deletion.remote_path) &&
                                    path[deletion.remote_path.size()] == '/';
            if (iterator->first == deletion.remote_id ||
                path == deletion.remote_path || descendant) {
                iterator = items.erase(iterator);
            } else {
                ++iterator;
            }
        }
        pending_deletes_by_id.erase(deletion.remote_id);
    }

    void save_pending_remote_move(onedrive::storage::PendingRemoteMove move) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_remote_move_save) {
            throw std::runtime_error{
                "simulated pending remote move persistence failure"
            };
        }
        pending_remote_moves_by_id.insert_or_assign(
            move.remote_id, std::move(move)
        );
    }

    void remove_pending_remote_move(
        const std::string&, const std::string& remote_id
    ) {
        const std::scoped_lock lock{mutex};
        pending_remote_moves_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingRemoteMove> result;
        for (const auto& [remote_id, move] : pending_remote_moves_by_id) {
            static_cast<void>(remote_id);
            result.push_back(move);
        }
        return result;
    }

    void commit_remote_move(
        const onedrive::storage::PendingRemoteMove& move,
        onedrive::storage::ItemState item
    ) {
        const std::scoped_lock lock{mutex};
        if (fail_commit_remote_move) {
            throw std::runtime_error{"simulated remote move commit failure"};
        }
        if (move.directory) {
            for (auto& [remote_id, state] : items) {
                if (remote_id == move.remote_id) {
                    continue;
                }
                const auto relative_remote =
                    std::filesystem::path{state.remote_path}.lexically_relative(
                        move.source_remote_path
                    );
                const auto relative_local =
                    state.local_path.lexically_relative(move.source_local_path);
                if (!relative_remote.empty() &&
                    !relative_remote.native().starts_with("..") &&
                    relative_remote == relative_local) {
                    state.remote_path =
                        (std::filesystem::path{move.destination_remote_path} /
                         relative_remote)
                            .generic_string();
                    state.local_path =
                        move.destination_local_path / relative_local;
                }
            }
        }
        items.insert_or_assign(item.remote_id, std::move(item));
        pending_remote_moves_by_id.erase(move.remote_id);
    }

    void save_pending_move(onedrive::storage::PendingMove move) {
        const std::scoped_lock lock{mutex};
        if (before_pending_move_save) {
            before_pending_move_save(move);
        }
        pending_moves_by_id.insert_or_assign(move.remote_id, std::move(move));
    }

    void remove_pending_move(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        if (fail_pending_move_remove) {
            throw std::runtime_error{"simulated move journal cleanup failure"};
        }
        removed_pending_moves.push_back(remote_id);
        pending_moves_by_id.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::PendingMove> result;
        for (const auto& [remote_id, move] : pending_moves_by_id) {
            static_cast<void>(remote_id);
            result.push_back(move);
        }
        return result;
    }

    [[nodiscard]] std::vector<onedrive::storage::UploadSuppression>
    upload_suppressions(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::UploadSuppression> result;
        for (const auto& [path, suppression] : upload_suppressions_by_path) {
            static_cast<void>(path);
            result.push_back(suppression);
        }
        if (reverse_state_order) {
            std::ranges::reverse(result);
        }
        return result;
    }

    void remove_upload_suppression(
        const std::string&, const std::filesystem::path& local_path
    ) {
        const std::scoped_lock lock{mutex};
        upload_suppressions_by_path.erase(
            local_path.lexically_normal().string()
        );
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem>
    blocked_items(const std::string&) const {
        const std::scoped_lock lock{mutex};
        auto result = blocked;
        if (reverse_state_order) {
            std::ranges::reverse(result);
        }
        return result;
    }

    void save_blocked_item(onedrive::storage::BlockedItem item) {
        const std::scoped_lock lock{mutex};
        const auto existing = std::ranges::find(
            blocked, item.remote_id, &onedrive::storage::BlockedItem::remote_id
        );
        item.attempt_count =
            existing == blocked.end() ? 1 : existing->attempt_count + 1;
        if (existing == blocked.end()) {
            blocked.push_back(std::move(item));
        } else {
            *existing = std::move(item);
        }
    }

    void remove_blocked_item(const std::string&, const std::string& remote_id) {
        const std::scoped_lock lock{mutex};
        std::erase_if(blocked, [&](const auto& item) {
            return item.remote_id == remote_id;
        });
    }

    bool reset(const std::string&) {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) {
        return {};
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string&) const {
        return saved_delta_link;
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string&) const {
        return saved_sync_filter_fingerprint;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState>
    find(const std::string&, const std::string& remote_id) const {
        const std::scoped_lock lock{mutex};
        const auto iterator = items.find(remote_id);
        return iterator == items.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::ItemState>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock{mutex};
        return items.size();
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState>
    drive_items(const std::string&) const {
        const std::scoped_lock lock{mutex};
        std::vector<onedrive::storage::ItemState> result;
        result.reserve(items.size());
        for (const auto& [remote_id, item] : items) {
            static_cast<void>(remote_id);
            result.push_back(item);
        }
        if (reverse_state_order) {
            std::ranges::reverse(result);
        }
        return result;
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> items;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::unordered_map<std::string, onedrive::storage::PartialDownload>
        partials;
    std::unordered_map<std::string, onedrive::storage::PendingUpload>
        pending_uploads_by_path;
    std::unordered_map<std::string, onedrive::storage::PendingDelete>
        pending_deletes_by_id;
    std::unordered_map<std::string, onedrive::storage::PendingRemoteMove>
        pending_remote_moves_by_id;
    std::unordered_map<std::string, onedrive::storage::PendingMove>
        pending_moves_by_id;
    std::unordered_map<std::string, onedrive::storage::UploadSuppression>
        upload_suppressions_by_path;
    std::vector<onedrive::storage::BlockedItem> blocked;
    onedrive::storage::ItemDelta applied_delta;
    std::optional<onedrive::storage::PendingDelta> pending_remote_delta;
    bool fail_save_pending_delta{false};
    std::optional<std::string> saved_delta_link;
    std::optional<std::string> saved_sync_filter_fingerprint{
        onedrive::sync::engine_detail::sync_scope_fingerprint("")
    };
    bool reverse_state_order{false};
    int upsert_count{0};
    int apply_count{0};
    bool fail_upsert{false};
    bool fail_commit_upload{false};
    bool fail_pending_upload_save{false};
    bool fail_commit_delete{false};
    bool fail_pending_delete_save{false};
    bool fail_commit_remote_move{false};
    bool fail_pending_remote_move_save{false};
    bool fail_upload_checkpoint_save{false};
    bool fail_apply_delta{false};
    bool fail_pending_move_remove{false};
    std::function<void(const onedrive::storage::PendingMove&)>
        before_pending_move_save;
    std::vector<std::string> removed_pending_moves;
    mutable std::mutex mutex;
};

} // namespace onedrive::test::sync
