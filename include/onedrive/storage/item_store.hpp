#pragma once

#include "onedrive/util/file_hash.hpp"
#include "onedrive/util/proxy_service.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <proxy/proxy.h>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::storage {

struct ItemState {
    std::string drive_id;
    std::string remote_id;
    std::string parent_id;
    std::string name;
    std::string etag;
    std::string ctag;
    std::string remote_path;
    std::filesystem::path local_path;
    std::string last_modified;
    std::int64_t size{0};
    std::int64_t local_size{0};
    std::int64_t local_modified_ticks{0};
    std::uint64_t local_device{0};
    std::uint64_t local_inode{0};
    std::optional<util::FileHash> content_hash;
    bool directory{false};
};

struct UploadSuppression {
    std::string drive_id;
    std::string remote_id;
    std::filesystem::path local_path;
    std::uint64_t source_device{0};
    std::uint64_t source_inode{0};
};

struct BlockedItem {
    std::string drive_id;
    std::string remote_id;
    std::string parent_id;
    std::string name;
    std::string etag;
    std::string ctag;
    std::string remote_path;
    std::string last_modified;
    std::int64_t size{0};
    bool directory{false};
    bool deleted{false};
    std::string reason_code;
    std::string reason_message;
    std::uint64_t attempt_count{0};
    std::optional<util::FileHash> content_hash;
};

enum class DeltaApplyMode {
    merge,
    replace,
};

struct ItemDelta {
    std::string drive_id;
    std::vector<ItemState> upserts;
    std::vector<std::string> removals;
    std::vector<std::string> partial_download_removals;
    std::vector<BlockedItem> blocked_upserts;
    std::vector<std::string> blocked_removals;
    std::vector<UploadSuppression> upload_suppressions;
    std::string delta_link;
    std::string sync_filter_fingerprint;
    DeltaApplyMode apply_mode{DeltaApplyMode::merge};
};

struct PendingDownload {
    ItemState item;
    std::filesystem::path temporary_path;
    std::string content_fingerprint;
    std::filesystem::path backup_path;
    std::string backup_fingerprint;
};

struct PartialDownload {
    ItemState item;
    std::filesystem::path temporary_path;
    std::uint64_t completed_bytes{0};
};

struct PendingUpload {
    std::string drive_id;
    std::string remote_path;
    std::filesystem::path local_path;
    std::filesystem::path snapshot_path;
    std::string content_fingerprint;
    std::int64_t local_size{0};
    std::int64_t local_modified_ticks{0};
    std::optional<std::string> remote_id;
    std::string expected_etag;
    std::string upload_url;
    std::string upload_expiration;
    std::uint64_t completed_bytes{0};
    std::string failure_code;
    std::string failure_message;
    std::uint64_t failure_attempt_count{0};
    bool directory{false};
};

struct PendingDelete {
    std::string drive_id;
    std::string remote_id;
    std::string expected_etag;
    std::string remote_path;
    std::filesystem::path local_path;
    bool directory{false};
};

struct PendingRemoteMove {
    std::string drive_id;
    std::string remote_id;
    std::string expected_etag;
    std::string source_remote_path;
    std::string destination_remote_path;
    std::filesystem::path source_local_path;
    std::filesystem::path destination_local_path;
    std::uint64_t local_device{0};
    std::uint64_t local_inode{0};
    bool directory{false};
};

struct PendingMove {
    std::string drive_id;
    std::string remote_id;
    std::filesystem::path source_path;
    std::filesystem::path destination_path;
    std::filesystem::path staging_path;
    std::uint64_t source_device{0};
    std::uint64_t source_inode{0};
    bool directory{false};
};

struct ClearedState {
    std::size_t items{0};
    std::size_t pending_downloads{0};
    std::size_t partial_downloads{0};
    std::size_t pending_uploads{0};
    std::size_t pending_deletes{0};
    std::size_t pending_remote_moves{0};
    std::size_t pending_moves{0};
    std::size_t upload_suppressions{0};
    std::size_t blocked_items{0};
    bool delta_link{false};
};

PRO_DEF_MEM_DISPATCH(StoreOpenDispatch, open);
PRO_DEF_MEM_DISPATCH(StoreOpenReadOnlyDispatch, open_read_only);
PRO_DEF_MEM_DISPATCH(StoreUpsertDispatch, upsert);
PRO_DEF_MEM_DISPATCH(StoreApplyDeltaDispatch, apply_delta);
PRO_DEF_MEM_DISPATCH(StoreSavePendingDispatch, save_pending_download);
PRO_DEF_MEM_DISPATCH(StoreRemovePendingDispatch, remove_pending_download);
PRO_DEF_MEM_DISPATCH(StorePendingDispatch, pending_downloads);
PRO_DEF_MEM_DISPATCH(StoreSavePartialDispatch, save_partial_download);
PRO_DEF_MEM_DISPATCH(StoreRemovePartialDispatch, remove_partial_download);
PRO_DEF_MEM_DISPATCH(StorePartialDispatch, partial_download);
PRO_DEF_MEM_DISPATCH(StorePartialsDispatch, partial_downloads);
PRO_DEF_MEM_DISPATCH(StoreSavePendingUploadDispatch, save_pending_upload);
PRO_DEF_MEM_DISPATCH(StoreRemovePendingUploadDispatch, remove_pending_upload);
PRO_DEF_MEM_DISPATCH(StorePendingUploadsDispatch, pending_uploads);
PRO_DEF_MEM_DISPATCH(StoreCommitUploadDispatch, commit_upload);
PRO_DEF_MEM_DISPATCH(StoreSavePendingDeleteDispatch, save_pending_delete);
PRO_DEF_MEM_DISPATCH(StoreRemovePendingDeleteDispatch, remove_pending_delete);
PRO_DEF_MEM_DISPATCH(StorePendingDeletesDispatch, pending_deletes);
PRO_DEF_MEM_DISPATCH(StoreCommitDeleteDispatch, commit_delete);
PRO_DEF_MEM_DISPATCH(
    StoreSavePendingRemoteMoveDispatch, save_pending_remote_move
);
PRO_DEF_MEM_DISPATCH(
    StoreRemovePendingRemoteMoveDispatch, remove_pending_remote_move
);
PRO_DEF_MEM_DISPATCH(StorePendingRemoteMovesDispatch, pending_remote_moves);
PRO_DEF_MEM_DISPATCH(StoreCommitRemoteMoveDispatch, commit_remote_move);
PRO_DEF_MEM_DISPATCH(StoreSavePendingMoveDispatch, save_pending_move);
PRO_DEF_MEM_DISPATCH(StoreRemovePendingMoveDispatch, remove_pending_move);
PRO_DEF_MEM_DISPATCH(StorePendingMovesDispatch, pending_moves);
PRO_DEF_MEM_DISPATCH(StoreUploadSuppressionsDispatch, upload_suppressions);
PRO_DEF_MEM_DISPATCH(
    StoreRemoveUploadSuppressionDispatch, remove_upload_suppression
);
PRO_DEF_MEM_DISPATCH(StoreBlockedDispatch, blocked_items);
PRO_DEF_MEM_DISPATCH(StoreResetDispatch, reset);
PRO_DEF_MEM_DISPATCH(StoreClearDispatch, clear);
PRO_DEF_MEM_DISPATCH(StoreDeltaLinkDispatch, delta_link);
PRO_DEF_MEM_DISPATCH(
    StoreSyncFilterFingerprintDispatch, sync_filter_fingerprint
);
PRO_DEF_MEM_DISPATCH(StoreFindDispatch, find);
PRO_DEF_MEM_DISPATCH(StoreDriveItemsDispatch, drive_items);
PRO_DEF_MEM_DISPATCH(StoreSizeDispatch, size);

struct ItemStoreFacade : pro::facade_builder ::add_convention<StoreOpenDispatch, void()>::add_convention<StoreOpenReadOnlyDispatch, void()>::add_convention<
                             StoreUpsertDispatch,
                             void(ItemState
                             )>::add_convention<StoreApplyDeltaDispatch, void(ItemDelta)>::
                             add_convention<StoreSavePendingDispatch, void(PendingDownload)>::add_convention<
                                 StoreRemovePendingDispatch,
                                 void(const std::string&, const std::string&)>::
                                 add_convention<
                                     StorePendingDispatch,
                                     std::vector<PendingDownload>(
                                         const std::string&
                                     ) const>::
                                     add_convention<StoreSavePartialDispatch, void(PartialDownload)>::add_convention<
                                         StoreRemovePartialDispatch,
                                         void(
                                             const std::string&,
                                             const std::string&
                                         )>::
                                         add_convention<
                                             StorePartialDispatch,
                                             std::optional<PartialDownload>(
                                                 const std::string&,
                                                 const std::string&
                                             ) const>::
                                             add_convention<
                                                 StorePartialsDispatch,
                                                 std::vector<PartialDownload>(
                                                     const std::string&
                                                 ) const>::add_convention<StoreSavePendingUploadDispatch, void(PendingUpload)>::
                                                 add_convention<
                                                     StoreRemovePendingUploadDispatch,
                                                     void(
                                                         const std::string&,
                                                         const std::string&
                                                     )>::
                                                     add_convention<
                                                         StorePendingUploadsDispatch,
                                                         std::vector<
                                                             PendingUpload>(
                                                             const std::string&
                                                         ) const>::
                                                         add_convention<
                                                             StoreCommitUploadDispatch,
                                                             void(
                                                                 const PendingUpload&,
                                                                 ItemState
                                                             )>::add_convention<StoreSavePendingDeleteDispatch, void(PendingDelete)>::
                                                             add_convention<
                                                                 StoreRemovePendingDeleteDispatch,
                                                                 void(
                                                                     const std::string&,
                                                                     const std::string&
                                                                 )>::
                                                                 add_convention<
                                                                     StorePendingDeletesDispatch,
                                                                     std::
                                                                         vector<PendingDelete>(const std::
                                                                                                   string&
                                                                         ) const>::
                                                                     add_convention<
                                                                         StoreCommitDeleteDispatch,
                                                                         void(
                                                                             const PendingDelete&
                                                                         )>::add_convention<StoreSavePendingRemoteMoveDispatch, void(PendingRemoteMove)>::add_convention<StoreRemovePendingRemoteMoveDispatch, void(const std::string&, const std::string&)>::add_convention<StorePendingRemoteMovesDispatch, std::vector<PendingRemoteMove>(const std::string&) const>::add_convention<StoreCommitRemoteMoveDispatch, void(const PendingRemoteMove&, ItemState)>::add_convention<StoreSavePendingMoveDispatch, void(PendingMove)>::add_convention<StoreRemovePendingMoveDispatch, void(const std::string&, const std::string&)>::add_convention<StorePendingMovesDispatch, std::vector<PendingMove>(const std::string&) const>::add_convention<StoreUploadSuppressionsDispatch, std::vector<UploadSuppression>(const std::string&) const>::add_convention<StoreRemoveUploadSuppressionDispatch, void(const std::string&, const std::filesystem::path&)>::add_convention<StoreBlockedDispatch, std::vector<BlockedItem>(const std::string&) const>::
                                                                         add_convention<
                                                                             StoreResetDispatch,
                                                                             bool(
                                                                                 const std::string&
                                                                             )>::
                                                                             add_convention<
                                                                                 StoreClearDispatch,
                                                                                 ClearedState(
                                                                                     const std::string&
                                                                                 )>::
                                                                                 add_convention<
                                                                                     StoreDeltaLinkDispatch,
                                                                                     std::
                                                                                         optional<std::string>(const std::string&
                                                                                         ) const>::
                                                                                     add_convention<
                                                                                         StoreSyncFilterFingerprintDispatch,
                                                                                         std::
                                                                                             optional<std::string>(const std::string&
                                                                                             ) const>::
                                                                                         add_convention<
                                                                                             StoreFindDispatch,
                                                                                             std::
                                                                                                 optional<
                                                                                                     ItemState>(const std::string&, const std::string&)
                                                                                                     const>::
                                                                                             add_convention<
                                                                                                 StoreDriveItemsDispatch,
                                                                                                 std::vector<
                                                                                                     ItemState>(const std::string&
                                                                                                 ) const>::
                                                                                                 add_convention<
                                                                                                     StoreSizeDispatch,
                                                                                                     std::size_t(
                                                                                                     ) const>::
                                                                                                     build {
};

class ItemStore : private onedrive::util::ProxyService<ItemStoreFacade> {
    using Base = onedrive::util::ProxyService<ItemStoreFacade>;

public:
    using Base::Base;

    void open() {
        implementation()->open();
    }

    void open_read_only() {
        implementation()->open_read_only();
    }

    void upsert(ItemState item) {
        implementation()->upsert(std::move(item));
    }

    void apply_delta(ItemDelta delta) {
        implementation()->apply_delta(std::move(delta));
    }

    void save_pending_download(PendingDownload download) {
        implementation()->save_pending_download(std::move(download));
    }

    void remove_pending_download(
        const std::string& drive_id, const std::string& remote_id
    ) {
        implementation()->remove_pending_download(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PendingDownload>
    pending_downloads(const std::string& drive_id) const {
        return implementation()->pending_downloads(drive_id);
    }

    void save_partial_download(PartialDownload download) {
        implementation()->save_partial_download(std::move(download));
    }

    void remove_partial_download(
        const std::string& drive_id, const std::string& remote_id
    ) {
        implementation()->remove_partial_download(drive_id, remote_id);
    }

    [[nodiscard]] std::optional<PartialDownload> partial_download(
        const std::string& drive_id, const std::string& remote_id
    ) const {
        return implementation()->partial_download(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PartialDownload>
    partial_downloads(const std::string& drive_id) const {
        return implementation()->partial_downloads(drive_id);
    }

    void save_pending_upload(PendingUpload upload) {
        implementation()->save_pending_upload(std::move(upload));
    }

    void remove_pending_upload(
        const std::string& drive_id, const std::string& remote_path
    ) {
        implementation()->remove_pending_upload(drive_id, remote_path);
    }

    [[nodiscard]] std::vector<PendingUpload>
    pending_uploads(const std::string& drive_id) const {
        return implementation()->pending_uploads(drive_id);
    }

    void commit_upload(const PendingUpload& upload, ItemState item) {
        implementation()->commit_upload(upload, std::move(item));
    }

    void save_pending_delete(PendingDelete deletion) {
        implementation()->save_pending_delete(std::move(deletion));
    }

    void remove_pending_delete(
        const std::string& drive_id, const std::string& remote_id
    ) {
        implementation()->remove_pending_delete(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PendingDelete>
    pending_deletes(const std::string& drive_id) const {
        return implementation()->pending_deletes(drive_id);
    }

    void commit_delete(const PendingDelete& deletion) {
        implementation()->commit_delete(deletion);
    }

    void save_pending_remote_move(PendingRemoteMove move) {
        implementation()->save_pending_remote_move(std::move(move));
    }

    void remove_pending_remote_move(
        const std::string& drive_id, const std::string& remote_id
    ) {
        implementation()->remove_pending_remote_move(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PendingRemoteMove>
    pending_remote_moves(const std::string& drive_id) const {
        return implementation()->pending_remote_moves(drive_id);
    }

    void commit_remote_move(const PendingRemoteMove& move, ItemState item) {
        implementation()->commit_remote_move(move, std::move(item));
    }

    void save_pending_move(PendingMove move) {
        implementation()->save_pending_move(std::move(move));
    }

    void remove_pending_move(
        const std::string& drive_id, const std::string& remote_id
    ) {
        implementation()->remove_pending_move(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PendingMove>
    pending_moves(const std::string& drive_id) const {
        return implementation()->pending_moves(drive_id);
    }

    [[nodiscard]] std::vector<UploadSuppression>
    upload_suppressions(const std::string& drive_id) const {
        return implementation()->upload_suppressions(drive_id);
    }

    void remove_upload_suppression(
        const std::string& drive_id, const std::filesystem::path& local_path
    ) {
        implementation()->remove_upload_suppression(drive_id, local_path);
    }

    [[nodiscard]] std::vector<BlockedItem>
    blocked_items(const std::string& drive_id) const {
        return implementation()->blocked_items(drive_id);
    }

    bool reset(const std::string& drive_id) {
        return implementation()->reset(drive_id);
    }

    ClearedState clear(const std::string& drive_id) {
        return implementation()->clear(drive_id);
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string& drive_id) const {
        return implementation()->delta_link(drive_id);
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string& drive_id) const {
        return implementation()->sync_filter_fingerprint(drive_id);
    }

    [[nodiscard]] std::optional<ItemState>
    find(const std::string& drive_id, const std::string& remote_id) const {
        return implementation()->find(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<ItemState>
    drive_items(const std::string& drive_id) const {
        return implementation()->drive_items(drive_id);
    }

    [[nodiscard]] std::size_t size() const {
        return implementation()->size();
    }
};

} // namespace onedrive::storage
