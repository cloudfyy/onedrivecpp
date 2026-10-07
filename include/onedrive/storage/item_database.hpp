#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace onedrive::storage {

struct DatabaseIntegrityResult {
    std::filesystem::path path;
    bool healthy{false};
    std::string detail;
};

[[nodiscard]] std::vector<DatabaseIntegrityResult>
diagnose_state_databases(const std::filesystem::path& state_directory);

class ItemDatabase final {
public:
    ItemDatabase(
        std::filesystem::path state_directory, account::DriveIdentity identity
    );
    ~ItemDatabase();

    ItemDatabase(const ItemDatabase&) = delete;
    ItemDatabase& operator=(const ItemDatabase&) = delete;
    ItemDatabase(ItemDatabase&&) = delete;
    ItemDatabase& operator=(ItemDatabase&&) = delete;

    void open();
    void open_read_only();
    void upsert(ItemState item);
    void apply_delta(ItemDelta delta);
    void save_pending_download(PendingDownload download);
    void remove_pending_download(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingDownload>
    pending_downloads(const std::string& drive_id) const;
    void save_partial_download(PartialDownload download);
    void remove_partial_download(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::optional<PartialDownload> partial_download(
        const std::string& drive_id, const std::string& remote_id
    ) const;
    [[nodiscard]] std::vector<PartialDownload>
    partial_downloads(const std::string& drive_id) const;
    void save_pending_upload(PendingUpload upload);
    void remove_pending_upload(
        const std::string& drive_id, const std::string& remote_path
    );
    [[nodiscard]] std::vector<PendingUpload>
    pending_uploads(const std::string& drive_id) const;
    void commit_upload(const PendingUpload& upload, ItemState item);
    void save_pending_delete(PendingDelete deletion);
    void remove_pending_delete(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingDelete>
    pending_deletes(const std::string& drive_id) const;
    void commit_delete(const PendingDelete& deletion);
    void save_pending_remote_move(PendingRemoteMove move);
    void remove_pending_remote_move(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingRemoteMove>
    pending_remote_moves(const std::string& drive_id) const;
    void commit_remote_move(const PendingRemoteMove& move, ItemState item);
    void save_pending_move(PendingMove move);
    void remove_pending_move(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingMove>
    pending_moves(const std::string& drive_id) const;
    [[nodiscard]] std::vector<UploadSuppression>
    upload_suppressions(const std::string& drive_id) const;
    void remove_upload_suppression(
        const std::string& drive_id, const std::filesystem::path& local_path
    );
    [[nodiscard]] std::vector<BlockedItem>
    blocked_items(const std::string& drive_id) const;
    bool reset(const std::string& drive_id);
    ClearedState clear(const std::string& drive_id);
    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string& drive_id) const;
    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string& drive_id) const;
    [[nodiscard]] std::optional<ItemState>
    find(const std::string& drive_id, const std::string& remote_id) const;
    [[nodiscard]] std::vector<ItemState>
    drive_items(const std::string& drive_id) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Impl;

    enum class CorruptionRecovery {
        quarantine_and_rebuild,
        fail,
    };

    void open_on_worker(CorruptionRecovery recovery);
    void open_read_only_on_worker();
    void upsert_on_worker(const ItemState& item);
    void apply_delta_on_worker(ItemDelta delta);
    void save_pending_download_on_worker(const PendingDownload& download);
    void remove_pending_download_on_worker(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingDownload>
    pending_downloads_on_worker(const std::string& drive_id) const;
    void save_partial_download_on_worker(const PartialDownload& download);
    void remove_partial_download_on_worker(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::optional<PartialDownload> partial_download_on_worker(
        const std::string& drive_id, const std::string& remote_id
    ) const;
    [[nodiscard]] std::vector<PartialDownload>
    partial_downloads_on_worker(const std::string& drive_id) const;
    void save_pending_upload_on_worker(const PendingUpload& upload);
    void remove_pending_upload_on_worker(
        const std::string& drive_id, const std::string& remote_path
    );
    [[nodiscard]] std::vector<PendingUpload>
    pending_uploads_on_worker(const std::string& drive_id) const;
    void
    commit_upload_on_worker(const PendingUpload& upload, const ItemState& item);
    void save_pending_delete_on_worker(const PendingDelete& deletion);
    void remove_pending_delete_on_worker(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingDelete>
    pending_deletes_on_worker(const std::string& drive_id) const;
    void commit_delete_on_worker(const PendingDelete& deletion);
    void save_pending_remote_move_on_worker(const PendingRemoteMove& move);
    void remove_pending_remote_move_on_worker(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingRemoteMove>
    pending_remote_moves_on_worker(const std::string& drive_id) const;
    void commit_remote_move_on_worker(
        const PendingRemoteMove& move, const ItemState& item
    );
    void save_pending_move_on_worker(const PendingMove& move);
    void remove_pending_move_on_worker(
        const std::string& drive_id, const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingMove>
    pending_moves_on_worker(const std::string& drive_id) const;
    [[nodiscard]] std::vector<UploadSuppression>
    upload_suppressions_on_worker(const std::string& drive_id) const;
    void remove_upload_suppression_on_worker(
        const std::string& drive_id, const std::filesystem::path& local_path
    );
    [[nodiscard]] std::vector<BlockedItem>
    blocked_items_on_worker(const std::string& drive_id) const;
    bool reset_on_worker(const std::string& drive_id);
    ClearedState clear_on_worker(const std::string& drive_id);
    [[nodiscard]] std::optional<std::string>
    delta_link_on_worker(const std::string& drive_id) const;
    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint_on_worker(const std::string& drive_id) const;
    [[nodiscard]] std::optional<ItemState> find_on_worker(
        const std::string& drive_id, const std::string& remote_id
    ) const;
    [[nodiscard]] std::vector<ItemState>
    drive_items_on_worker(const std::string& drive_id) const;
    [[nodiscard]] std::size_t size_on_worker() const;

    std::filesystem::path state_directory_;
    account::DriveIdentity identity_;
    std::unique_ptr<Impl> impl_;
};

} // namespace onedrive::storage
