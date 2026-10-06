#include "onedrive/storage/item_database.hpp"
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

using item_database_detail::Statement;
using item_database_detail::Transaction;
using item_database_detail::bind_integer;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::query_count;

bool ItemDatabase::reset(const std::string& drive_id) {
    return impl_->invoke([this, drive_id] {
        return reset_on_worker(drive_id);
    });
}

bool ItemDatabase::reset_on_worker(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot reset synchronization state without a drive ID"
        );
    }

    Transaction transaction{database};
    Statement state_statement{
        database,
        "DELETE FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, state_statement.get(), 1, drive_id);
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot reset drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const bool had_delta_link = sqlite3_changes(database) != 0;
    transaction.commit();

    const auto retained_items = query_count(
        database,
        "SELECT COUNT(*) FROM item WHERE drive_id = ?1;",
        &drive_id
    );
    spdlog::debug(
        "Reset synchronization cursor for drive '{}': saved cursor {}, "
        "{} item snapshots retained",
        drive_id,
        had_delta_link ? "removed" : "not present",
        retained_items
    );
    return had_delta_link;
}

ClearedState ItemDatabase::clear(const std::string& drive_id) {
    return impl_->invoke([this, drive_id] {
        return clear_on_worker(drive_id);
    });
}

ClearedState ItemDatabase::clear_on_worker(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot clear synchronization state without a drive ID"
        );
    }

    ClearedState cleared;
    Transaction transaction{database};
    Statement item_statement{
        database,
        "DELETE FROM item WHERE drive_id = ?1;"
    };
    bind_text(database, item_statement.get(), 1, drive_id);
    if (sqlite3_step(item_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear drive items: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.items = static_cast<std::size_t>(sqlite3_changes(database));

    Statement state_statement{
        database,
        "DELETE FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, state_statement.get(), 1, drive_id);
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.delta_link = sqlite3_changes(database) != 0;

    Statement pending_statement{
        database,
        "DELETE FROM pending_download WHERE drive_id = ?1;"
    };
    bind_text(database, pending_statement.get(), 1, drive_id);
    if (sqlite3_step(pending_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending downloads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_downloads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement partial_statement{
        database,
        "DELETE FROM partial_download WHERE drive_id = ?1;"
    };
    bind_text(database, partial_statement.get(), 1, drive_id);
    if (sqlite3_step(partial_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear partial downloads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.partial_downloads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement upload_statement{
        database,
        "DELETE FROM pending_upload WHERE drive_id = ?1;"
    };
    bind_text(database, upload_statement.get(), 1, drive_id);
    if (sqlite3_step(upload_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending uploads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_uploads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement delete_statement{
        database,
        "DELETE FROM pending_delete WHERE drive_id = ?1;"
    };
    bind_text(database, delete_statement.get(), 1, drive_id);
    if (sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending deletions: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_deletes =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement remote_move_statement{
        database,
        "DELETE FROM pending_remote_move WHERE drive_id = ?1;"
    };
    bind_text(database, remote_move_statement.get(), 1, drive_id);
    if (sqlite3_step(remote_move_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending remote moves: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_remote_moves =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement move_statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1;"
    };
    bind_text(database, move_statement.get(), 1, drive_id);
    if (sqlite3_step(move_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending moves: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_moves =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement suppression_statement{
        database,
        "DELETE FROM upload_suppression WHERE drive_id = ?1;"
    };
    bind_text(database, suppression_statement.get(), 1, drive_id);
    if (sqlite3_step(suppression_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear upload suppressions: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.upload_suppressions =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement blocked_statement{
        database,
        "DELETE FROM blocked_item WHERE drive_id = ?1;"
    };
    bind_text(database, blocked_statement.get(), 1, drive_id);
    if (sqlite3_step(blocked_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear blocked items: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.blocked_items =
        static_cast<std::size_t>(sqlite3_changes(database));
    transaction.commit();

    spdlog::warn(
        "Cleared all synchronization state for drive '{}': {} item snapshots, "
        "{} pending downloads, {} partial downloads, {} pending uploads, {} "
        "pending deletions, {} pending remote moves, {} pending moves, {} "
        "upload suppressions, {} "
        "blocked items, saved delta cursor {}",
        drive_id,
        cleared.items,
        cleared.pending_downloads,
        cleared.partial_downloads,
        cleared.pending_uploads,
        cleared.pending_deletes,
        cleared.pending_remote_moves,
        cleared.pending_moves,
        cleared.upload_suppressions,
        cleared.blocked_items,
        cleared.delta_link ? "removed" : "not present"
    );
    return cleared;
}

}  // namespace onedrive::storage
