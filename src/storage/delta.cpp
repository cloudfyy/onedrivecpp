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
using item_database_detail::query_count;
using item_database_detail::serialize_file_hash;
using item_database_detail::Statement;
using item_database_detail::Transaction;

void ItemDatabase::apply_delta(ItemDelta delta) {
    impl_->invoke([this, delta = std::move(delta)]() mutable {
        apply_delta_on_worker(std::move(delta));
    });
}

void ItemDatabase::apply_delta_on_worker(ItemDelta delta) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (delta.drive_id.empty() || delta.delta_link.empty()) {
        throw std::invalid_argument(
            "delta state requires a drive ID and delta link"
        );
    }

    Transaction transaction{database};
    if (delta.apply_mode == DeltaApplyMode::replace) {
        Statement replace_statement{
            database, "DELETE FROM item WHERE drive_id = ?1;"
        };
        bind_text(database, replace_statement.get(), 1, delta.drive_id);
        replace_statement.step_done("cannot replace drive items");
        Statement replace_blocked_statement{
            database, "DELETE FROM blocked_item WHERE drive_id = ?1;"
        };
        bind_text(database, replace_blocked_statement.get(), 1, delta.drive_id);
        replace_blocked_statement.step_done(
            "cannot replace blocked drive items"
        );
    }
    Statement upsert_statement{
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
    for (auto& item : delta.upserts) {
        if (item.remote_id.empty()) {
            throw std::invalid_argument(
                "cannot persist a delta item without an ID"
            );
        }
        item.drive_id = delta.drive_id;
        const std::string local_path = item.local_path.string();
        bind_text(database, upsert_statement.get(), 1, item.drive_id);
        bind_text(database, upsert_statement.get(), 2, item.remote_id);
        bind_text(database, upsert_statement.get(), 3, item.parent_id);
        bind_text(database, upsert_statement.get(), 4, item.name);
        bind_text(database, upsert_statement.get(), 5, item.etag);
        bind_text(database, upsert_statement.get(), 6, item.ctag);
        bind_text(database, upsert_statement.get(), 7, item.remote_path);
        bind_text(database, upsert_statement.get(), 8, local_path);
        bind_text(database, upsert_statement.get(), 9, item.last_modified);
        bind_integer(database, upsert_statement.get(), 10, item.size);
        bind_integer(database, upsert_statement.get(), 11, item.local_size);
        bind_integer(
            database, upsert_statement.get(), 12, item.local_modified_ticks
        );
        bind_integer(
            database,
            upsert_statement.get(),
            13,
            static_cast<std::int64_t>(item.local_device)
        );
        bind_integer(
            database,
            upsert_statement.get(),
            14,
            static_cast<std::int64_t>(item.local_inode)
        );
        const auto [hash_algorithm, hash_value] =
            serialize_file_hash(item.content_hash);
        bind_text(database, upsert_statement.get(), 15, hash_algorithm);
        bind_text(database, upsert_statement.get(), 16, hash_value);
        bind_integer(
            database, upsert_statement.get(), 17, item.directory ? 1 : 0
        );
        upsert_statement.step_done("cannot apply delta item");
        upsert_statement.reset_for_reuse();
    }

    Statement complete_move_statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& item : delta.upserts) {
        bind_text(database, complete_move_statement.get(), 1, delta.drive_id);
        bind_text(database, complete_move_statement.get(), 2, item.remote_id);
        complete_move_statement.step_done(
            "cannot complete pending move journal"
        );
        complete_move_statement.reset_for_reuse();
    }
    for (const auto& remote_id : delta.removals) {
        bind_text(database, complete_move_statement.get(), 1, delta.drive_id);
        bind_text(database, complete_move_statement.get(), 2, remote_id);
        complete_move_statement.step_done(
            "cannot discard removed pending move journal"
        );
        complete_move_statement.reset_for_reuse();
    }

    Statement delete_statement{
        database, "DELETE FROM item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument(
                "cannot remove a delta item without an ID"
            );
        }
        bind_text(database, delete_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_statement.get(), 2, remote_id);
        delete_statement.step_done("cannot remove delta item");
        delete_statement.reset_for_reuse();
    }

    Statement delete_partial_statement{
        database,
        "DELETE FROM partial_download "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.partial_download_removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument(
                "cannot remove a partial download without an ID"
            );
        }
        bind_text(database, delete_partial_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_partial_statement.get(), 2, remote_id);
        delete_partial_statement.step_done("cannot remove partial download");
        delete_partial_statement.reset_for_reuse();
    }

    Statement suppression_statement{
        database,
        "INSERT INTO upload_suppression ("
        "drive_id, remote_id, local_path, source_device, source_inode"
        ") VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(drive_id, local_path) DO UPDATE SET "
        "remote_id = excluded.remote_id, "
        "source_device = excluded.source_device, "
        "source_inode = excluded.source_inode;"
    };
    for (auto& suppression : delta.upload_suppressions) {
        if (suppression.remote_id.empty() || suppression.local_path.empty() ||
            suppression.source_device >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()
                ) ||
            suppression.source_inode >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()
                )) {
            throw std::invalid_argument(
                "upload suppression contains invalid metadata"
            );
        }
        suppression.drive_id = delta.drive_id;
        bind_text(
            database, suppression_statement.get(), 1, suppression.drive_id
        );
        bind_text(
            database, suppression_statement.get(), 2, suppression.remote_id
        );
        bind_text(
            database,
            suppression_statement.get(),
            3,
            suppression.local_path.string()
        );
        bind_integer(
            database,
            suppression_statement.get(),
            4,
            static_cast<std::int64_t>(suppression.source_device)
        );
        bind_integer(
            database,
            suppression_statement.get(),
            5,
            static_cast<std::int64_t>(suppression.source_inode)
        );
        suppression_statement.step_done("cannot persist upload suppression");
        suppression_statement.reset_for_reuse();
    }

    Statement delete_blocked_statement{
        database,
        "DELETE FROM blocked_item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.blocked_removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument(
                "cannot remove a blocked item without an ID"
            );
        }
        bind_text(database, delete_blocked_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_blocked_statement.get(), 2, remote_id);
        delete_blocked_statement.step_done("cannot remove blocked item");
        delete_blocked_statement.reset_for_reuse();
    }

    Statement upsert_blocked_statement{
        database,
        "INSERT INTO blocked_item ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "last_modified, size, directory, deleted, reason_code, reason_message, "
        "content_hash_algorithm, content_hash_value"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15"
        ") "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, ctag = excluded.ctag, "
        "remote_path = excluded.remote_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, deleted = excluded.deleted, "
        "reason_code = excluded.reason_code, "
        "reason_message = excluded.reason_message, "
        "content_hash_algorithm = excluded.content_hash_algorithm, "
        "content_hash_value = excluded.content_hash_value, "
        "last_attempt = unixepoch(), "
        "attempt_count = blocked_item.attempt_count + 1;"
    };
    for (auto& item : delta.blocked_upserts) {
        if (item.remote_id.empty() || item.reason_code.empty() ||
            item.reason_message.empty()) {
            throw std::invalid_argument(
                "blocked item requires an ID, reason code, and reason message"
            );
        }
        item.drive_id = delta.drive_id;
        bind_text(database, upsert_blocked_statement.get(), 1, item.drive_id);
        bind_text(database, upsert_blocked_statement.get(), 2, item.remote_id);
        bind_text(database, upsert_blocked_statement.get(), 3, item.parent_id);
        bind_text(database, upsert_blocked_statement.get(), 4, item.name);
        bind_text(database, upsert_blocked_statement.get(), 5, item.etag);
        bind_text(database, upsert_blocked_statement.get(), 6, item.ctag);
        bind_text(
            database, upsert_blocked_statement.get(), 7, item.remote_path
        );
        bind_text(
            database, upsert_blocked_statement.get(), 8, item.last_modified
        );
        bind_integer(database, upsert_blocked_statement.get(), 9, item.size);
        bind_integer(
            database, upsert_blocked_statement.get(), 10, item.directory ? 1 : 0
        );
        bind_integer(
            database, upsert_blocked_statement.get(), 11, item.deleted ? 1 : 0
        );
        bind_text(
            database, upsert_blocked_statement.get(), 12, item.reason_code
        );
        bind_text(
            database, upsert_blocked_statement.get(), 13, item.reason_message
        );
        std::string hash_algorithm;
        std::string hash_value;
        if (item.content_hash.has_value()) {
            hash_algorithm =
                item.content_hash->algorithm == util::FileHashAlgorithm::sha256
                    ? "sha256"
                    : "quick_xor";
            hash_value = item.content_hash->value;
            if (hash_value.empty()) {
                throw std::invalid_argument(
                    "blocked item content hash cannot be empty"
                );
            }
        }
        bind_text(database, upsert_blocked_statement.get(), 14, hash_algorithm);
        bind_text(database, upsert_blocked_statement.get(), 15, hash_value);
        upsert_blocked_statement.step_done("cannot persist blocked item");
        upsert_blocked_statement.reset_for_reuse();
    }

    Statement state_statement{
        database,
        "INSERT INTO drive_state ("
        "drive_id, delta_link, sync_filter_fingerprint"
        ") VALUES (?1, ?2, ?3) "
        "ON CONFLICT(drive_id) DO UPDATE SET "
        "delta_link = excluded.delta_link, "
        "sync_filter_fingerprint = excluded.sync_filter_fingerprint;"
    };
    bind_text(database, state_statement.get(), 1, delta.drive_id);
    bind_text(database, state_statement.get(), 2, delta.delta_link);
    bind_text(
        database, state_statement.get(), 3, delta.sync_filter_fingerprint
    );
    state_statement.step_done("cannot update delta link");
    transaction.commit();

    spdlog::debug(
        "Committed remote delta for drive '{}': {} upserts, {} removals, {} "
        "blocked, inventory {}, delta cursor advanced, {} total items tracked",
        delta.drive_id,
        delta.upserts.size(),
        delta.removals.size(),
        delta.blocked_upserts.size(),
        delta.apply_mode == DeltaApplyMode::replace ? "replaced" : "updated",
        query_count(database, "SELECT COUNT(*) FROM item;")
    );
}

} // namespace onedrive::storage
