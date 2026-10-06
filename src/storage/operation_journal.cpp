#include "onedrive/storage/item_database.hpp"
#include "storage/database_worker.hpp"
#include "storage/sqlite_support.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
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

std::vector<UploadSuppression> ItemDatabase::upload_suppressions(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return upload_suppressions_on_worker(drive_id);
    });
}

std::vector<UploadSuppression>
ItemDatabase::upload_suppressions_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, local_path, source_device, source_inode "
        "FROM upload_suppression WHERE drive_id = ?1 ORDER BY local_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<UploadSuppression> suppressions;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read upload suppressions: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const auto device = sqlite3_column_int64(statement.get(), 3);
        const auto inode = sqlite3_column_int64(statement.get(), 4);
        if (device < 0 || inode < 0) {
            throw std::runtime_error(
                "upload suppression contains invalid filesystem identity"
            );
        }
        suppressions.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .local_path = column_text(statement.get(), 2),
            .source_device = static_cast<std::uint64_t>(device),
            .source_inode = static_cast<std::uint64_t>(inode),
        });
    }
    return suppressions;
}

void ItemDatabase::remove_upload_suppression(
    const std::string& drive_id,
    const std::filesystem::path& local_path
) {
    impl_->invoke([this, drive_id, local_path] {
        remove_upload_suppression_on_worker(drive_id, local_path);
    });
}

void ItemDatabase::remove_upload_suppression_on_worker(
    const std::string& drive_id,
    const std::filesystem::path& local_path
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty() || local_path.empty()) {
        throw std::invalid_argument(
            "upload suppression removal requires drive and local path"
        );
    }
    Statement statement{
        database,
        "DELETE FROM upload_suppression "
        "WHERE drive_id = ?1 AND local_path = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, local_path.string());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove upload suppression: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<BlockedItem> ItemDatabase::blocked_items(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return blocked_items_on_worker(drive_id);
    });
}

std::vector<BlockedItem> ItemDatabase::blocked_items_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "last_modified, size, directory, deleted, reason_code, reason_message, "
        "attempt_count, content_hash_algorithm, content_hash_value "
        "FROM blocked_item WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<BlockedItem> result;
    while (true) {
        const int step_result = sqlite3_step(statement.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read blocked synchronization items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const std::string hash_algorithm = column_text(statement.get(), 14);
        const std::string hash_value = column_text(statement.get(), 15);
        std::optional<util::FileHash> content_hash;
        if (!hash_algorithm.empty() || !hash_value.empty()) {
            if (hash_value.empty() ||
                (hash_algorithm != "sha256" &&
                 hash_algorithm != "quick_xor")) {
                throw std::runtime_error(
                    "blocked item contains invalid content hash metadata"
                );
            }
            content_hash = util::FileHash{
                .algorithm = hash_algorithm == "sha256" ?
                    util::FileHashAlgorithm::sha256 :
                    util::FileHashAlgorithm::quick_xor,
                .value = hash_value,
            };
        }
        result.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .parent_id = column_text(statement.get(), 2),
            .name = column_text(statement.get(), 3),
            .etag = column_text(statement.get(), 4),
            .ctag = column_text(statement.get(), 5),
            .remote_path = column_text(statement.get(), 6),
            .last_modified = column_text(statement.get(), 7),
            .size = sqlite3_column_int64(statement.get(), 8),
            .directory = sqlite3_column_int(statement.get(), 9) != 0,
            .deleted = sqlite3_column_int(statement.get(), 10) != 0,
            .reason_code = column_text(statement.get(), 11),
            .reason_message = column_text(statement.get(), 12),
            .attempt_count = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 13)
            ),
            .content_hash = std::move(content_hash),
        });
    }
    return result;
}

}  // namespace onedrive::storage
