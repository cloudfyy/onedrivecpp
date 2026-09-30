#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace onedrive::storage {

struct ItemState {
    std::string remote_id;
    std::string etag;
    std::filesystem::path local_path;
};

class ItemDatabase {
public:
    explicit ItemDatabase(std::filesystem::path state_directory);
    ~ItemDatabase();

    ItemDatabase(const ItemDatabase&) = delete;
    ItemDatabase& operator=(const ItemDatabase&) = delete;

    void open();
    void upsert(ItemState item);
    [[nodiscard]] const ItemState* find(const std::string& remote_id) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Impl;

    std::filesystem::path state_directory_;
    std::unordered_map<std::string, ItemState> items_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace onedrive::storage
