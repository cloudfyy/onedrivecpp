#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace onedrive::storage {

struct ItemState {
    std::string remote_id;
    std::string etag;
    std::filesystem::path local_path;
};

class ItemStore {
public:
    virtual ~ItemStore() = default;

    virtual void open() = 0;
    virtual void upsert(ItemState item) = 0;
    [[nodiscard]] virtual const ItemState* find(
        const std::string& remote_id
    ) const = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

}  // namespace onedrive::storage
