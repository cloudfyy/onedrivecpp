#include "onedrive/storage/status.hpp"

#include "storage/query.hpp"
#include "storage/schema.hpp"
#include "storage/sqlite.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace onedrive::storage {
namespace {

using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::query_count;
using item_database_detail::SqliteHandle;
using item_database_detail::Statement;
using item_database_detail::verify_current_schema;

std::size_t count_for_drive(
    sqlite3* database, const char* table, const std::string& drive_id
) {
    const std::string query =
        "SELECT COUNT(*) FROM " + std::string{table} + " WHERE drive_id = ?1;";
    return query_count(database, query.c_str(), drive_id);
}

} // namespace

StateSummary read_state_summary(
    const std::filesystem::path& drive_state_directory,
    const std::string& drive_id
) {
    const auto database_path = drive_state_directory / "items.sqlite3";
    if (!std::filesystem::exists(database_path)) {
        return {};
    }
    sqlite3* raw_database = nullptr;
    const int result = sqlite3_open_v2(
        database_path.c_str(),
        &raw_database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
        nullptr
    );
    SqliteHandle database{raw_database};
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot open synchronization state database read-only: " +
            std::string{
                raw_database == nullptr ? sqlite3_errstr(result)
                                        : sqlite3_errmsg(raw_database)
            }
        );
    }
    verify_current_schema(database.get());

    StateSummary summary{
        .database_present = true,
        .tracked_items = count_for_drive(database.get(), "item", drive_id),
        .blocked_items =
            count_for_drive(database.get(), "blocked_item", drive_id),
        .pending_downloads =
            count_for_drive(database.get(), "pending_download", drive_id),
        .partial_downloads =
            count_for_drive(database.get(), "partial_download", drive_id),
        .pending_uploads =
            count_for_drive(database.get(), "pending_upload", drive_id),
        .pending_deletes =
            count_for_drive(database.get(), "pending_delete", drive_id),
        .pending_remote_moves =
            count_for_drive(database.get(), "pending_remote_move", drive_id),
        .pending_local_moves =
            count_for_drive(database.get(), "pending_move", drive_id),
        .delta_cursor = false,
        .sync_filter_fingerprint = {},
    };
    Statement statement{
        database.get(),
        "SELECT delta_link, sync_filter_fingerprint FROM drive_state "
        "WHERE drive_id = ?1;"
    };
    bind_text(database.get(), statement.get(), 1, drive_id);
    if (statement.next_row("cannot read synchronization drive state")) {
        summary.delta_cursor = !column_text(statement.get(), 0).empty();
        summary.sync_filter_fingerprint = column_text(statement.get(), 1);
    }
    return summary;
}

} // namespace onedrive::storage
