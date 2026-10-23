#pragma once

#include "onedrive/storage/item_store.hpp"
#include "storage/file_hash.hpp"
#include "storage/sqlite.hpp"

#include <cstdint>
#include <optional>

namespace onedrive::storage::item_database_detail {

inline constexpr char item_columns[] =
    "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
    "local_path, last_modified, size, local_size, local_modified_ticks, "
    "local_device, local_inode, content_hash_algorithm, "
    "content_hash_value, directory";

inline constexpr char item_upsert_sql[] =
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
    "directory = excluded.directory;";

// Snapshots and download journals share their first ten columns.
inline void bind_item_prefix(
    Database database, PreparedStatement statement, const ItemState& item
) {
    bind_text(database, statement, 1, item.drive_id);
    bind_text(database, statement, 2, item.remote_id);
    bind_text(database, statement, 3, item.parent_id);
    bind_text(database, statement, 4, item.name);
    bind_text(database, statement, 5, item.etag);
    bind_text(database, statement, 6, item.ctag);
    bind_text(database, statement, 7, item.remote_path);
    bind_text(database, statement, 8, item.local_path.string());
    bind_text(database, statement, 9, item.last_modified);
    bind_integer(database, statement, 10, item.size);
}

inline ItemState read_item_prefix(PreparedStatement statement) {
    return {
        .drive_id = column_text(statement, 0),
        .remote_id = column_text(statement, 1),
        .parent_id = column_text(statement, 2),
        .name = column_text(statement, 3),
        .etag = column_text(statement, 4),
        .ctag = column_text(statement, 5),
        .remote_path = column_text(statement, 6),
        .local_path = column_text(statement, 7),
        .last_modified = column_text(statement, 8),
        .size = sqlite3_column_int64(statement, 9),
        .content_hash = std::nullopt,
    };
}

inline void bind_item(
    Database database, PreparedStatement statement, const ItemState& item
) {
    bind_item_prefix(database, statement, item);
    bind_integer(database, statement, 11, item.local_size);
    bind_integer(database, statement, 12, item.local_modified_ticks);
    bind_integer(
        database, statement, 13, static_cast<std::int64_t>(item.local_device)
    );
    bind_integer(
        database, statement, 14, static_cast<std::int64_t>(item.local_inode)
    );
    const auto [algorithm, value] = serialize_file_hash(item.content_hash);
    bind_text(database, statement, 15, algorithm);
    bind_text(database, statement, 16, value);
    bind_integer(database, statement, 17, item.directory ? 1 : 0);
}

inline ItemState read_item(PreparedStatement statement) {
    auto item = read_item_prefix(statement);
    item.local_size = sqlite3_column_int64(statement, 10);
    item.local_modified_ticks = sqlite3_column_int64(statement, 11);
    item.local_device =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 12));
    item.local_inode =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement, 13));
    item.content_hash = parse_file_hash(
        column_text(statement, 14), column_text(statement, 15), "item"
    );
    item.directory = sqlite3_column_int(statement, 16) != 0;
    return item;
}

inline void bind_download_item(
    Database database, PreparedStatement statement, const ItemState& item
) {
    bind_item_prefix(database, statement, item);
    bind_integer(database, statement, 11, item.directory ? 1 : 0);
}

inline ItemState read_download_item(PreparedStatement statement) {
    auto item = read_item_prefix(statement);
    item.directory = sqlite3_column_int(statement, 10) != 0;
    return item;
}

} // namespace onedrive::storage::item_database_detail
