#pragma once

#include "onedrive/storage/item_store.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::test::app {

class FakeItemStore final {
public:
    FakeItemStore(
        int& open_count,
        int& apply_delta_count,
        int& reset_count,
        std::string& reset_drive_id,
        int& clear_count,
        std::string& clear_drive_id,
        const std::vector<onedrive::storage::ItemState>& items,
        const std::vector<onedrive::storage::PartialDownload>& partials,
        const std::vector<onedrive::storage::PendingDownload>& pending,
        std::vector<onedrive::storage::BlockedItem>& blocked
    )
        : open_count_{open_count},
          apply_delta_count_{apply_delta_count},
          reset_count_{reset_count},
          reset_drive_id_{reset_drive_id},
          clear_count_{clear_count},
          clear_drive_id_{clear_drive_id},
          items_{items},
          partials_{partials},
          pending_{pending},
          blocked_{blocked} {
    }

    void open() {
        ++open_count_;
    }

    void open_read_only() {
        ++open_count_;
    }

    void upsert(onedrive::storage::ItemState) {
    }

    void apply_delta(onedrive::storage::ItemDelta) {
        ++apply_delta_count_;
    }

    void save_pending_delta(onedrive::storage::PendingDelta) {
    }
    [[nodiscard]] std::optional<onedrive::storage::PendingDelta>
    pending_delta(const std::string&) const {
        return std::nullopt;
    }
    void remove_pending_delta(const std::string&) {
    }

    void save_pending_download(onedrive::storage::PendingDownload) {
    }

    void remove_pending_download(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string& drive_id) const {
        auto filtered =
            pending_ | std::views::filter([&](const auto& download) {
                return download.item.drive_id == drive_id;
            });
        return {filtered.begin(), filtered.end()};
    }

    void save_partial_download(onedrive::storage::PartialDownload) {
    }

    void remove_partial_download(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(const std::string&, const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::vector<onedrive::storage::PartialDownload>
    partial_downloads(const std::string& drive_id) const {
        auto filtered =
            partials_ | std::views::filter([&](const auto& partial) {
                return partial.item.drive_id == drive_id;
            });
        return {filtered.begin(), filtered.end()};
    }

    void save_pending_upload(onedrive::storage::PendingUpload) {
    }
    void remove_pending_upload(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        return {};
    }

    void commit_upload(
        const onedrive::storage::PendingUpload&, onedrive::storage::ItemState
    ) {
    }

    void save_pending_delete(onedrive::storage::PendingDelete) {
    }
    void remove_pending_delete(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingDelete>
    pending_deletes(const std::string&) const {
        return {};
    }
    void commit_delete(const onedrive::storage::PendingDelete&) {
    }
    void save_pending_remote_move(onedrive::storage::PendingRemoteMove) {
    }
    void remove_pending_remote_move(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        return {};
    }
    void commit_remote_move(
        const onedrive::storage::PendingRemoteMove&,
        onedrive::storage::ItemState
    ) {
    }

    void save_pending_move(onedrive::storage::PendingMove) {
    }

    void remove_pending_move(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        return {};
    }

    [[nodiscard]] std::vector<onedrive::storage::UploadSuppression>
    upload_suppressions(const std::string&) const {
        return {};
    }

    void remove_upload_suppression(
        const std::string&, const std::filesystem::path&
    ) {
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem>
    blocked_items(const std::string& drive_id) const {
        auto filtered = blocked_ | std::views::filter([&](const auto& item) {
                            return item.drive_id == drive_id;
                        });
        return {filtered.begin(), filtered.end()};
    }

    void save_blocked_item(onedrive::storage::BlockedItem item) {
        const auto existing =
            std::ranges::find_if(blocked_, [&](const auto& saved) {
                return saved.drive_id == item.drive_id &&
                       saved.remote_id == item.remote_id;
            });
        item.attempt_count =
            existing == blocked_.end() ? 1 : existing->attempt_count + 1;
        if (existing == blocked_.end()) {
            blocked_.push_back(std::move(item));
        } else {
            *existing = std::move(item);
        }
    }

    void remove_blocked_item(
        const std::string& drive_id, const std::string& remote_id
    ) {
        std::erase_if(blocked_, [&](const auto& item) {
            return item.drive_id == drive_id && item.remote_id == remote_id;
        });
    }

    bool reset(const std::string& drive_id) {
        ++reset_count_;
        reset_drive_id_ = drive_id;
        return true;
    }

    onedrive::storage::ClearedState clear(const std::string& drive_id) {
        ++clear_count_;
        clear_drive_id_ = drive_id;
        return {
            .items = 7,
            .pending_downloads = 2,
            .partial_downloads = 4,
            .pending_uploads = 1,
            .pending_moves = 5,
            .upload_suppressions = 6,
            .blocked_items = 3,
            .delta_link = true,
        };
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState>
    find(const std::string&, const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return 0;
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState>
    drive_items(const std::string& drive_id) const {
        auto filtered = items_ | std::views::filter([&](const auto& item) {
                            return item.drive_id == drive_id;
                        });
        return {filtered.begin(), filtered.end()};
    }

private:
    int& open_count_;
    int& apply_delta_count_;
    int& reset_count_;
    std::string& reset_drive_id_;
    int& clear_count_;
    std::string& clear_drive_id_;
    const std::vector<onedrive::storage::ItemState>& items_;
    const std::vector<onedrive::storage::PartialDownload>& partials_;
    const std::vector<onedrive::storage::PendingDownload>& pending_;
    std::vector<onedrive::storage::BlockedItem>& blocked_;
};

} // namespace onedrive::test::app
