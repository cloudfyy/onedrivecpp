#include "onedrive/storage/item_database.hpp"
#include "storage/file_hash.hpp"
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

using item_database_detail::bind_integer;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::parse_file_hash;
using item_database_detail::query_count;
using item_database_detail::serialize_file_hash;
using item_database_detail::Statement;

void ItemDatabase::upsert(ItemState item) {
    impl_->invoke([this, item = std::move(item)] { upsert_on_worker(item); });
}

void ItemDatabase::upsert_on_worker(const ItemState& item) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database,
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, content_hash_algorithm, "
        "content_hash_value, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13, ?14, ?15, ?16, ?17) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, ctag = excluded.ctag, "
        "remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "local_size = excluded.local_size, "
        "local_modified_ticks = excluded.local_modified_ticks, "
        "local_device = excluded.local_device, "
        "local_inode = excluded.local_inode, "
        "content_hash_algorithm = excluded.content_hash_algorithm, "
        "content_hash_value = excluded.content_hash_value, "
        "directory = excluded.directory;"
    };
    const std::string local_path = item.local_path.string();
    bind_text(database, statement.get(), 1, item.drive_id);
    bind_text(database, statement.get(), 2, item.remote_id);
    bind_text(database, statement.get(), 3, item.parent_id);
    bind_text(database, statement.get(), 4, item.name);
    bind_text(database, statement.get(), 5, item.etag);
    bind_text(database, statement.get(), 6, item.ctag);
    bind_text(database, statement.get(), 7, item.remote_path);
    bind_text(database, statement.get(), 8, local_path);
    bind_text(database, statement.get(), 9, item.last_modified);
    bind_integer(database, statement.get(), 10, item.size);
    bind_integer(database, statement.get(), 11, item.local_size);
    bind_integer(database, statement.get(), 12, item.local_modified_ticks);
    bind_integer(
        database,
        statement.get(),
        13,
        static_cast<std::int64_t>(item.local_device)
    );
    bind_integer(
        database,
        statement.get(),
        14,
        static_cast<std::int64_t>(item.local_inode)
    );
    const auto [hash_algorithm, hash_value] =
        serialize_file_hash(item.content_hash);
    bind_text(database, statement.get(), 15, hash_algorithm);
    bind_text(database, statement.get(), 16, hash_value);
    bind_integer(database, statement.get(), 17, item.directory ? 1 : 0);

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
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, content_hash_algorithm, "
        "content_hash_value, directory FROM item "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (!statement.next_row("cannot read synchronization item")) {
        return std::nullopt;
    }
    return ItemState{
        .drive_id = column_text(statement.get(), 0),
        .remote_id = column_text(statement.get(), 1),
        .parent_id = column_text(statement.get(), 2),
        .name = column_text(statement.get(), 3),
        .etag = column_text(statement.get(), 4),
        .ctag = column_text(statement.get(), 5),
        .remote_path = column_text(statement.get(), 6),
        .local_path = column_text(statement.get(), 7),
        .last_modified = column_text(statement.get(), 8),
        .size = sqlite3_column_int64(statement.get(), 9),
        .local_size = sqlite3_column_int64(statement.get(), 10),
        .local_modified_ticks = sqlite3_column_int64(statement.get(), 11),
        .local_device =
            static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 12)
            ),
        .local_inode =
            static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 13)
            ),
        .content_hash = parse_file_hash(
            column_text(statement.get(), 14),
            column_text(statement.get(), 15),
            "item"
        ),
        .directory = sqlite3_column_int(statement.get(), 16) != 0,
    };
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
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, content_hash_algorithm, "
        "content_hash_value, directory FROM item "
        "WHERE drive_id = ?1 ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<ItemState> result;
    while (statement.next_row("cannot read synchronization drive items")) {
        result.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .parent_id = column_text(statement.get(), 2),
            .name = column_text(statement.get(), 3),
            .etag = column_text(statement.get(), 4),
            .ctag = column_text(statement.get(), 5),
            .remote_path = column_text(statement.get(), 6),
            .local_path = column_text(statement.get(), 7),
            .last_modified = column_text(statement.get(), 8),
            .size = sqlite3_column_int64(statement.get(), 9),
            .local_size = sqlite3_column_int64(statement.get(), 10),
            .local_modified_ticks = sqlite3_column_int64(statement.get(), 11),
            .local_device = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 12)
            ),
            .local_inode = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 13)
            ),
            .content_hash = parse_file_hash(
                column_text(statement.get(), 14),
                column_text(statement.get(), 15),
                "item"
            ),
            .directory = sqlite3_column_int(statement.get(), 16) != 0,
        });
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
