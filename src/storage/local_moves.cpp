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

void ItemDatabase::save_pending_move(PendingMove move) {
    impl_->invoke([this, move = std::move(move)] {
        save_pending_move_on_worker(move);
    });
}

void ItemDatabase::save_pending_move_on_worker(
    const PendingMove& move
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (move.drive_id.empty() || move.remote_id.empty() ||
        move.source_path.empty() || move.destination_path.empty() ||
        move.source_path == move.destination_path ||
        (!move.staging_path.empty() &&
         (move.staging_path == move.source_path ||
          move.staging_path == move.destination_path)) ||
        move.source_device >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            ) ||
        move.source_inode >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
        throw std::invalid_argument("pending move contains invalid metadata");
    }
    Statement statement{
        database,
        "INSERT INTO pending_move ("
        "drive_id, remote_id, source_path, destination_path, staging_path, "
        "source_device, source_inode, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "source_path = excluded.source_path, "
        "destination_path = excluded.destination_path, "
        "staging_path = excluded.staging_path, "
        "source_device = excluded.source_device, "
        "source_inode = excluded.source_inode, "
        "directory = excluded.directory;"
    };
    bind_text(database, statement.get(), 1, move.drive_id);
    bind_text(database, statement.get(), 2, move.remote_id);
    bind_text(database, statement.get(), 3, move.source_path.string());
    bind_text(
        database,
        statement.get(),
        4,
        move.destination_path.string()
    );
    bind_text(database, statement.get(), 5, move.staging_path.string());
    bind_integer(
        database,
        statement.get(),
        6,
        static_cast<std::int64_t>(move.source_device)
    );
    bind_integer(
        database,
        statement.get(),
        7,
        static_cast<std::int64_t>(move.source_inode)
    );
    bind_integer(database, statement.get(), 8, move.directory ? 1 : 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_pending_move(
    const std::string& drive_id,
    const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_move_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_move_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingMove> ItemDatabase::pending_moves(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_moves_on_worker(drive_id);
    });
}

std::vector<PendingMove> ItemDatabase::pending_moves_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, source_path, destination_path, "
        "staging_path, source_device, source_inode, directory "
        "FROM pending_move "
        "WHERE drive_id = ?1 ORDER BY remote_id;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingMove> moves;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending moves: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const auto device = sqlite3_column_int64(statement.get(), 5);
        const auto inode = sqlite3_column_int64(statement.get(), 6);
        if (device < 0 || inode < 0) {
            throw std::runtime_error(
                "pending move contains invalid filesystem identity"
            );
        }
        moves.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .source_path = column_text(statement.get(), 2),
            .destination_path = column_text(statement.get(), 3),
            .staging_path = column_text(statement.get(), 4),
            .source_device = static_cast<std::uint64_t>(device),
            .source_inode = static_cast<std::uint64_t>(inode),
            .directory = sqlite3_column_int(statement.get(), 7) != 0,
        });
    }
    return moves;
}

}  // namespace onedrive::storage
