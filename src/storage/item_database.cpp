#include "onedrive/storage/item_database.hpp"

#include <utility>

namespace onedrive::storage {

ItemDatabase::ItemDatabase(std::filesystem::path state_directory)
    : state_directory_{std::move(state_directory)} {}

void ItemDatabase::open() {
    std::filesystem::create_directories(state_directory_);
}

void ItemDatabase::upsert(ItemState item) {
    items_.insert_or_assign(item.remote_id, std::move(item));
}

const ItemState* ItemDatabase::find(const std::string& remote_id) const {
    const auto iterator = items_.find(remote_id);
    return iterator == items_.end() ? nullptr : &iterator->second;
}

std::size_t ItemDatabase::size() const noexcept {
    return items_.size();
}

}  // namespace onedrive::storage
