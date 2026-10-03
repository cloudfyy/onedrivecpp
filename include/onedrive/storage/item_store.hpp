#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

class ItemStore {
public:
    ItemStore() = default;
    virtual ~ItemStore() = default;
    ItemStore(const ItemStore&) = delete;
    ItemStore& operator=(const ItemStore&) = delete;
    ItemStore(ItemStore&&) = delete;
    ItemStore& operator=(ItemStore&&) = delete;

    virtual void open() = 0;
    virtual void upsert(ItemState item) = 0;
    virtual void apply_delta(ItemDelta delta) = 0;
    virtual void save_pending_download(PendingDownload download) = 0;
    virtual void remove_pending_download(
        const std::string& drive_id,
        const std::string& remote_id
    ) = 0;
    [[nodiscard]] virtual std::vector<PendingDownload> pending_downloads(
        const std::string& drive_id
    ) const = 0;
    [[nodiscard]] virtual std::vector<BlockedItem> blocked_items(
        const std::string& drive_id
    ) const = 0;
    virtual bool reset(const std::string& drive_id) = 0;
    virtual ClearedState clear(const std::string& drive_id) = 0;
    [[nodiscard]] virtual std::optional<std::string> delta_link(
        const std::string& drive_id
    ) const = 0;
    [[nodiscard]] virtual std::optional<ItemState> find(
        const std::string& drive_id,
        const std::string& remote_id
    ) const = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

}  // namespace onedrive::storage
