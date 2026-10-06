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

void ItemDatabase::save_pending_remote_move(PendingRemoteMove move) {
    impl_->invoke([this, move = std::move(move)] {
        save_pending_remote_move_on_worker(move);
    });
}

void ItemDatabase::save_pending_remote_move_on_worker(
    const PendingRemoteMove& move
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (move.drive_id.empty() || move.remote_id.empty() ||
        move.expected_etag.empty() || move.source_remote_path.empty() ||
        move.destination_remote_path.empty() ||
        move.source_remote_path == move.destination_remote_path ||
        move.source_local_path.empty() ||
        move.destination_local_path.empty() ||
        move.local_device == 0 || move.local_inode == 0 ||
        move.local_device >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            ) ||
        move.local_inode >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
        throw std::invalid_argument(
            "pending remote move contains invalid metadata"
        );
    }
    Statement statement{
        database,
        "INSERT OR REPLACE INTO pending_remote_move ("
        "drive_id, remote_id, expected_etag, source_remote_path, "
        "destination_remote_path, source_local_path, destination_local_path, "
        "local_device, local_inode, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);"
    };
    bind_text(database, statement.get(), 1, move.drive_id);
    bind_text(database, statement.get(), 2, move.remote_id);
    bind_text(database, statement.get(), 3, move.expected_etag);
    bind_text(database, statement.get(), 4, move.source_remote_path);
    bind_text(database, statement.get(), 5, move.destination_remote_path);
    bind_text(
        database,
        statement.get(),
        6,
        move.source_local_path.string()
    );
    bind_text(
        database,
        statement.get(),
        7,
        move.destination_local_path.string()
    );
    bind_integer(
        database,
        statement.get(),
        8,
        static_cast<std::int64_t>(move.local_device)
    );
    bind_integer(
        database,
        statement.get(),
        9,
        static_cast<std::int64_t>(move.local_inode)
    );
    bind_integer(database, statement.get(), 10, move.directory ? 1 : 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending remote move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_pending_remote_move(
    const std::string& drive_id,
    const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_remote_move_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_remote_move_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_remote_move "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending remote move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingRemoteMove> ItemDatabase::pending_remote_moves(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_remote_moves_on_worker(drive_id);
    });
}

std::vector<PendingRemoteMove>
ItemDatabase::pending_remote_moves_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, expected_etag, source_remote_path, "
        "destination_remote_path, source_local_path, destination_local_path, "
        "local_device, local_inode, directory FROM pending_remote_move "
        "WHERE drive_id = ?1 ORDER BY source_remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingRemoteMove> moves;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending remote moves: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        moves.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .expected_etag = column_text(statement.get(), 2),
            .source_remote_path = column_text(statement.get(), 3),
            .destination_remote_path = column_text(statement.get(), 4),
            .source_local_path = column_text(statement.get(), 5),
            .destination_local_path = column_text(statement.get(), 6),
            .local_device = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 7)
            ),
            .local_inode = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 8)
            ),
            .directory = sqlite3_column_int(statement.get(), 9) != 0,
        });
    }
    return moves;
}

void ItemDatabase::commit_remote_move(
    const PendingRemoteMove& move,
    ItemState item
) {
    impl_->invoke([this, move, item = std::move(item)] {
        commit_remote_move_on_worker(move, item);
    });
}

void ItemDatabase::commit_remote_move_on_worker(
    const PendingRemoteMove& move,
    const ItemState& item
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (move.drive_id != item.drive_id ||
        move.remote_id != item.remote_id ||
        move.destination_remote_path != item.remote_path ||
        move.destination_local_path != item.local_path) {
        throw std::invalid_argument(
            "pending remote move and item state do not match"
        );
    }
    Transaction transaction{database};
    upsert_on_worker(item);
    if (move.directory) {
        Statement descendants{
            database,
            "UPDATE item SET "
            "remote_path = ?1 || substr(remote_path, length(?2) + 1), "
            "local_path = ?3 || substr(local_path, length(?4) + 1) "
            "WHERE drive_id = ?5 AND remote_id != ?6 AND "
            "substr(remote_path, 1, length(?2) + 1) = ?2 || '/' AND "
            "substr(local_path, 1, length(?4) + 1) = ?4 || '/';"
        };
        bind_text(
            database,
            descendants.get(),
            1,
            move.destination_remote_path
        );
        bind_text(
            database,
            descendants.get(),
            2,
            move.source_remote_path
        );
        bind_text(
            database,
            descendants.get(),
            3,
            move.destination_local_path.string()
        );
        bind_text(
            database,
            descendants.get(),
            4,
            move.source_local_path.string()
        );
        bind_text(database, descendants.get(), 5, move.drive_id);
        bind_text(database, descendants.get(), 6, move.remote_id);
        if (sqlite3_step(descendants.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot remap moved directory descendants: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
    }
    Statement journal{
        database,
        "DELETE FROM pending_remote_move "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, journal.get(), 1, move.drive_id);
    bind_text(database, journal.get(), 2, move.remote_id);
    if (sqlite3_step(journal.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
        throw std::runtime_error(
            "cannot complete pending remote move journal: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();
}

}  // namespace onedrive::storage
