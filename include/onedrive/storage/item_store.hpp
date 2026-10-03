#pragma once

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
    std::string remote_path;
    std::filesystem::path local_path;
    std::string last_modified;
    std::int64_t size{0};
    std::int64_t local_size{0};
    std::int64_t local_modified_ticks{0};
    bool directory{false};
};

struct BlockedItem {
    std::string drive_id;
    std::string remote_id;
    std::string parent_id;
    std::string name;
    std::string etag;
    std::string remote_path;
    std::string last_modified;
    std::int64_t size{0};
    bool directory{false};
    std::string reason_code;
    std::string reason_message;
    std::uint64_t attempt_count{0};
};

struct ItemDelta {
    std::string drive_id;
    std::vector<ItemState> upserts;
    std::vector<std::string> removals;
    std::vector<BlockedItem> blocked_upserts;
    std::vector<std::string> blocked_removals;
    std::string delta_link;
    bool replace_drive_items{false};
};

struct PendingDownload {
    ItemState item;
    std::filesystem::path temporary_path;
    std::string content_fingerprint;
};

struct ClearedState {
    std::size_t items{0};
    std::size_t pending_downloads{0};
    std::size_t blocked_items{0};
    bool delta_link{false};
};

PRO_DEF_MEM_DISPATCH(StoreOpenDispatch, open);
PRO_DEF_MEM_DISPATCH(StoreUpsertDispatch, upsert);
PRO_DEF_MEM_DISPATCH(StoreApplyDeltaDispatch, apply_delta);
PRO_DEF_MEM_DISPATCH(StoreSavePendingDispatch, save_pending_download);
PRO_DEF_MEM_DISPATCH(StoreRemovePendingDispatch, remove_pending_download);
PRO_DEF_MEM_DISPATCH(StorePendingDispatch, pending_downloads);
PRO_DEF_MEM_DISPATCH(StoreBlockedDispatch, blocked_items);
PRO_DEF_MEM_DISPATCH(StoreResetDispatch, reset);
PRO_DEF_MEM_DISPATCH(StoreClearDispatch, clear);
PRO_DEF_MEM_DISPATCH(StoreDeltaLinkDispatch, delta_link);
PRO_DEF_MEM_DISPATCH(StoreFindDispatch, find);
PRO_DEF_MEM_DISPATCH(StoreSizeDispatch, size);

struct ItemStoreFacade : pro::facade_builder
    ::add_convention<StoreOpenDispatch, void()>
    ::add_convention<StoreUpsertDispatch, void(ItemState)>
    ::add_convention<StoreApplyDeltaDispatch, void(ItemDelta)>
    ::add_convention<StoreSavePendingDispatch, void(PendingDownload)>
    ::add_convention<
        StoreRemovePendingDispatch,
        void(const std::string&, const std::string&)
    >
    ::add_convention<
        StorePendingDispatch,
        std::vector<PendingDownload>(const std::string&) const
    >
    ::add_convention<
        StoreBlockedDispatch,
        std::vector<BlockedItem>(const std::string&) const
    >
    ::add_convention<StoreResetDispatch, bool(const std::string&)>
    ::add_convention<StoreClearDispatch, ClearedState(const std::string&)>
    ::add_convention<
        StoreDeltaLinkDispatch,
        std::optional<std::string>(const std::string&) const
    >
    ::add_convention<
        StoreFindDispatch,
        std::optional<ItemState>(
            const std::string&,
            const std::string&
        ) const
    >
    ::add_convention<StoreSizeDispatch, std::size_t() const noexcept>
    ::build {};

class ItemStore {
public:
    template <typename Implementation, typename... Args>
    explicit ItemStore(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              ItemStoreFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit ItemStore(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit ItemStore(Implementation& implementation)
        : implementation_{&implementation} {}

    ~ItemStore() = default;
    ItemStore(const ItemStore&) = delete;
    ItemStore& operator=(const ItemStore&) = delete;
    ItemStore(ItemStore&&) noexcept = default;
    ItemStore& operator=(ItemStore&&) noexcept = default;

    void open() {
        implementation_->open();
    }

    void upsert(ItemState item) {
        implementation_->upsert(std::move(item));
    }

    void apply_delta(ItemDelta delta) {
        implementation_->apply_delta(std::move(delta));
    }

    void save_pending_download(PendingDownload download) {
        implementation_->save_pending_download(std::move(download));
    }

    void remove_pending_download(
        const std::string& drive_id,
        const std::string& remote_id
    ) {
        implementation_->remove_pending_download(drive_id, remote_id);
    }

    [[nodiscard]] std::vector<PendingDownload> pending_downloads(
        const std::string& drive_id
    ) const {
        return implementation_->pending_downloads(drive_id);
    }

    [[nodiscard]] std::vector<BlockedItem> blocked_items(
        const std::string& drive_id
    ) const {
        return implementation_->blocked_items(drive_id);
    }

    bool reset(const std::string& drive_id) {
        return implementation_->reset(drive_id);
    }

    ClearedState clear(const std::string& drive_id) {
        return implementation_->clear(drive_id);
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string& drive_id
    ) const {
        return implementation_->delta_link(drive_id);
    }

    [[nodiscard]] std::optional<ItemState> find(
        const std::string& drive_id,
        const std::string& remote_id
    ) const {
        return implementation_->find(drive_id, remote_id);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return implementation_->size();
    }

private:
    pro::proxy<ItemStoreFacade> implementation_;
};

}  // namespace onedrive::storage
