#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

namespace onedrive::storage::item_database_detail {

inline constexpr int current_schema_version = 25;

[[nodiscard]] int schema_version(sqlite3* database);
void create_blocked_item_v5_schema(sqlite3* database);
void add_blocked_item_hash_columns(sqlite3* database);
void add_blocked_item_deleted_column(sqlite3* database);
void add_item_content_hash(sqlite3* database);
void create_identity_schema(sqlite3* database);
void create_drive_mapping_schema(sqlite3* database);
void create_partial_download_v8_schema(sqlite3* database);
void create_pending_upload_v13_schema(sqlite3* database);
void create_pending_move_v14_schema(sqlite3* database);
void create_upload_suppression_schema(sqlite3* database);
void create_pending_delete_schema(sqlite3* database);
void create_pending_remote_move_schema(sqlite3* database);
void set_schema_version(sqlite3* database, int version);
void ensure_current_schema(sqlite3* database);
[[nodiscard]] std::vector<std::string> user_tables(sqlite3* database);

}  // namespace onedrive::storage::item_database_detail
