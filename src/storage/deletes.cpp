#include "onedrive/storage/item_database.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
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

void ItemDatabase::save_pending_delete(PendingDelete deletion) {
    impl_->invoke([this, deletion = std::move(deletion)] {
        save_pending_delete_on_worker(deletion);
    });
}

void ItemDatabase::save_pending_delete_on_worker(
    const PendingDelete& deletion
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (deletion.drive_id.empty() || deletion.remote_id.empty() ||
        deletion.expected_etag.empty() || deletion.remote_path.empty() ||
        deletion.local_path.empty()) {
        throw std::invalid_argument(
            "pending deletion contains invalid metadata"
        );
    }
    Statement statement{
        database,
        "INSERT OR REPLACE INTO pending_delete ("
        "drive_id, remote_id, expected_etag, remote_path, local_path, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6);"
    };
    bind_text(database, statement.get(), 1, deletion.drive_id);
    bind_text(database, statement.get(), 2, deletion.remote_id);
    bind_text(database, statement.get(), 3, deletion.expected_etag);
    bind_text(database, statement.get(), 4, deletion.remote_path);
    bind_text(
        database,
        statement.get(),
        5,
        deletion.local_path.string()
    );
    if (sqlite3_bind_int(
            statement.get(),
            6,
            deletion.directory ? 1 : 0
        ) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending deletion: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_pending_delete(
    const std::string& drive_id,
    const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_delete_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_delete_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_delete WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending deletion: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingDelete> ItemDatabase::pending_deletes(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_deletes_on_worker(drive_id);
    });
}

std::vector<PendingDelete> ItemDatabase::pending_deletes_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, expected_etag, remote_path, local_path, "
        "directory FROM pending_delete WHERE drive_id = ?1 "
        "ORDER BY length(remote_path), remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingDelete> deletions;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending deletions: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        deletions.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .expected_etag = column_text(statement.get(), 2),
            .remote_path = column_text(statement.get(), 3),
            .local_path = column_text(statement.get(), 4),
            .directory = sqlite3_column_int(statement.get(), 5) != 0,
        });
    }
    return deletions;
}

void ItemDatabase::commit_delete(const PendingDelete& deletion) {
    impl_->invoke([this, deletion] {
        commit_delete_on_worker(deletion);
    });
}

void ItemDatabase::commit_delete_on_worker(
    const PendingDelete& deletion
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Transaction transaction{database};
    Statement remove_items{
        database,
        "DELETE FROM item WHERE drive_id = ?1 AND (remote_id = ?2 OR "
        "remote_path = ?3 OR substr(remote_path, 1, length(?3) + 1) = ?3 || "
        "'/');"
    };
    bind_text(database, remove_items.get(), 1, deletion.drive_id);
    bind_text(database, remove_items.get(), 2, deletion.remote_id);
    bind_text(database, remove_items.get(), 3, deletion.remote_path);
    if (sqlite3_step(remove_items.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove deleted item state: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    Statement remove_journal{
        database,
        "DELETE FROM pending_delete WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, remove_journal.get(), 1, deletion.drive_id);
    bind_text(database, remove_journal.get(), 2, deletion.remote_id);
    if (sqlite3_step(remove_journal.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
        throw std::runtime_error(
            "cannot complete pending deletion journal: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();
}

}  // namespace onedrive::storage
