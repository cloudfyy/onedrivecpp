#include "onedrive/storage/item_database.hpp"
#include "storage/item_mapping.hpp"
#include "storage/query.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::storage {

using item_database_detail::bind_item;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::item_columns;
using item_database_detail::item_upsert_sql;
using item_database_detail::query_count;
using item_database_detail::read_item;
using item_database_detail::Statement;

void ItemDatabase::upsert(ItemState item) {
    impl_->invoke([this, item = std::move(item)] { upsert_on_worker(item); });
}

void ItemDatabase::upsert_on_worker(const ItemState& item) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{database, item_upsert_sql};
    bind_item(database, statement.get(), item);

    statement.execute("cannot update state database");
    spdlog::trace(
        "Updated synchronization state for drive '{}', item '{}'",
        item.drive_id,
        item.remote_id
    );
}

std::optional<std::string>
ItemDatabase::delta_link(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return delta_link_on_worker(drive_id);
    });
}

std::optional<std::string>
ItemDatabase::delta_link_on_worker(const std::string& drive_id) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database, "SELECT delta_link FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    if (!statement.next_row("cannot read drive delta link")) {
        return std::nullopt;
    }
    return column_text(statement.get(), 0);
}

std::optional<std::string>
ItemDatabase::sync_filter_fingerprint(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return sync_filter_fingerprint_on_worker(drive_id);
    });
}

std::optional<std::string> ItemDatabase::sync_filter_fingerprint_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT sync_filter_fingerprint FROM drive_state "
        "WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    if (!statement.next_row("cannot read sync filter fingerprint")) {
        return std::nullopt;
    }
    return column_text(statement.get(), 0);
}

std::optional<ItemState> ItemDatabase::find(
    const std::string& drive_id, const std::string& remote_id
) const {
    return impl_->invoke([this, drive_id, remote_id] {
        return find_on_worker(drive_id, remote_id);
    });
}

std::optional<ItemState> ItemDatabase::find_on_worker(
    const std::string& drive_id, const std::string& remote_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    const auto sql = "SELECT " + std::string{item_columns} +
                     " FROM item WHERE drive_id = ?1 AND remote_id = ?2;";
    Statement statement{database, sql.c_str()};
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (!statement.next_row("cannot read synchronization item")) {
        return std::nullopt;
    }
    return read_item(statement.get());
}

std::vector<ItemState>
ItemDatabase::drive_items(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return drive_items_on_worker(drive_id);
    });
}

std::vector<ItemState>
ItemDatabase::drive_items_on_worker(const std::string& drive_id) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    const auto sql = "SELECT " + std::string{item_columns} +
                     " FROM item WHERE drive_id = ?1 ORDER BY remote_path;";
    Statement statement{database, sql.c_str()};
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<ItemState> result;
    while (statement.next_row("cannot read synchronization drive items")) {
        result.push_back(read_item(statement.get()));
    }
    return result;
}

std::size_t ItemDatabase::size() const {
    return impl_->invoke([this] { return size_on_worker(); });
}

std::size_t ItemDatabase::size_on_worker() const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    return query_count(database, "SELECT COUNT(*) FROM item;");
}

} // namespace onedrive::storage
