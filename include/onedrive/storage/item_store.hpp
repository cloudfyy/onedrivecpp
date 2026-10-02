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

struct ItemDelta {
    std::string drive_id;
    std::vector<ItemState> upserts;
    std::vector<std::string> removals;
    std::string delta_link;
};

class ItemStore {
public:
    virtual ~ItemStore() = default;

    virtual void open() = 0;
    virtual void upsert(ItemState item) = 0;
    virtual void apply_delta(ItemDelta delta) = 0;
    virtual std::size_t reset(const std::string& drive_id) = 0;
    [[nodiscard]] virtual std::optional<std::string> delta_link(
        const std::string& drive_id
    ) const = 0;
    [[nodiscard]] virtual const ItemState* find(
        const std::string& drive_id,
        const std::string& remote_id
    ) const = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

}  // namespace onedrive::storage
