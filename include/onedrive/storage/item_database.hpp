#pragma once

#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace onedrive::storage {

class ItemDatabase final : public ItemStore {
public:
    explicit ItemDatabase(std::filesystem::path state_directory);
    ~ItemDatabase() override;

    ItemDatabase(const ItemDatabase&) = delete;
    ItemDatabase& operator=(const ItemDatabase&) = delete;

    void open() override;
    void upsert(ItemState item) override;
    void apply_delta(ItemDelta delta) override;
    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string& drive_id
    ) const override;
    [[nodiscard]] const ItemState* find(
        const std::string& remote_id
    ) const override;
    [[nodiscard]] std::size_t size() const noexcept override;

private:
    struct Impl;

    std::filesystem::path state_directory_;
    std::unordered_map<std::string, ItemState> items_;
    std::unordered_map<std::string, std::string> delta_links_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace onedrive::storage
