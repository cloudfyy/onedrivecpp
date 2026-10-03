#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace onedrive::storage {

class ItemDatabase final {
public:
    ItemDatabase(
        std::filesystem::path state_directory,
        account::DriveIdentity identity
    );
    ~ItemDatabase();

    ItemDatabase(const ItemDatabase&) = delete;
    ItemDatabase& operator=(const ItemDatabase&) = delete;
    ItemDatabase(ItemDatabase&&) = delete;
    ItemDatabase& operator=(ItemDatabase&&) = delete;

    void open();
    void upsert(ItemState item);
    void apply_delta(ItemDelta delta);
    void save_pending_download(PendingDownload download);
    void remove_pending_download(
        const std::string& drive_id,
        const std::string& remote_id
    );
    [[nodiscard]] std::vector<PendingDownload> pending_downloads(
        const std::string& drive_id
    ) const;
    [[nodiscard]] std::vector<BlockedItem> blocked_items(
        const std::string& drive_id
    ) const;
    bool reset(const std::string& drive_id);
    ClearedState clear(const std::string& drive_id);
    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string& drive_id
    ) const;
    [[nodiscard]] std::optional<ItemState> find(
        const std::string& drive_id,
        const std::string& remote_id
    ) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Impl;

    std::filesystem::path state_directory_;
    account::DriveIdentity identity_;
    std::unordered_map<std::string, ItemState> items_;
    std::unordered_map<std::string, BlockedItem> blocked_items_;
    std::unordered_map<std::string, std::string> delta_links_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace onedrive::storage
