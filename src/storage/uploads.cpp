#include "onedrive/storage/item_database.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

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

void ItemDatabase::save_pending_upload(PendingUpload upload) {
    impl_->invoke([this, upload = std::move(upload)] {
        save_pending_upload_on_worker(upload);
    });
}

void ItemDatabase::save_pending_upload_on_worker(
    const PendingUpload& upload
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    const bool has_failure = !upload.failure_code.empty();
    const bool invalid_failure =
        has_failure != !upload.failure_message.empty() ||
        has_failure != (upload.failure_attempt_count != 0);
    const bool invalid_directory =
        upload.directory &&
        (!upload.snapshot_path.empty() ||
         !upload.content_fingerprint.empty() ||
         upload.local_size != 0 ||
         upload.remote_id ||
         !upload.expected_etag.empty() ||
         !upload.upload_url.empty() ||
         !upload.upload_expiration.empty() ||
         upload.completed_bytes != 0);
    const bool invalid_file =
        !upload.directory &&
        ((!has_failure && upload.snapshot_path.empty()) ||
         (upload.snapshot_path.empty() !=
              upload.content_fingerprint.empty()) ||
         upload.local_size < 0 ||
         upload.remote_id.has_value() != !upload.expected_etag.empty() ||
         upload.completed_bytes >
             static_cast<std::uint64_t>(upload.local_size) ||
         (upload.upload_url.empty() != upload.upload_expiration.empty()) ||
         (upload.upload_url.empty() && upload.completed_bytes != 0));
    if (upload.drive_id.empty() || upload.remote_path.empty() ||
        upload.local_path.empty() || invalid_failure ||
        invalid_directory || invalid_file) {
        throw std::invalid_argument("pending upload contains invalid metadata");
    }
    Statement statement{
        database,
        "INSERT INTO pending_upload ("
        "drive_id, remote_path, local_path, snapshot_path, "
        "content_fingerprint, local_size, local_modified_ticks, remote_id, "
        "expected_etag, upload_url, upload_expiration, completed_bytes, "
        "failure_code, failure_message, failure_attempt_count, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13, ?14, ?15, ?16) "
        "ON CONFLICT(drive_id, remote_path) DO UPDATE SET "
        "local_path = excluded.local_path, "
        "snapshot_path = excluded.snapshot_path, "
        "content_fingerprint = excluded.content_fingerprint, "
        "local_size = excluded.local_size, "
        "local_modified_ticks = excluded.local_modified_ticks, "
        "remote_id = excluded.remote_id, "
        "expected_etag = excluded.expected_etag, "
        "upload_url = excluded.upload_url, "
        "upload_expiration = excluded.upload_expiration, "
        "completed_bytes = excluded.completed_bytes, "
        "failure_code = excluded.failure_code, "
        "failure_message = excluded.failure_message, "
        "failure_attempt_count = excluded.failure_attempt_count, "
        "directory = excluded.directory;"
    };
    bind_text(database, statement.get(), 1, upload.drive_id);
    bind_text(database, statement.get(), 2, upload.remote_path);
    bind_text(database, statement.get(), 3, upload.local_path.string());
    bind_text(database, statement.get(), 4, upload.snapshot_path.string());
    bind_text(database, statement.get(), 5, upload.content_fingerprint);
    bind_integer(database, statement.get(), 6, upload.local_size);
    bind_integer(database, statement.get(), 7, upload.local_modified_ticks);
    bind_text(
        database,
        statement.get(),
        8,
        upload.remote_id.value_or("")
    );
    bind_text(database, statement.get(), 9, upload.expected_etag);
    bind_text(database, statement.get(), 10, upload.upload_url);
    bind_text(database, statement.get(), 11, upload.upload_expiration);
    bind_integer(
        database,
        statement.get(),
        12,
        static_cast<std::int64_t>(upload.completed_bytes)
    );
    bind_text(database, statement.get(), 13, upload.failure_code);
    bind_text(database, statement.get(), 14, upload.failure_message);
    bind_integer(
        database,
        statement.get(),
        15,
        static_cast<std::int64_t>(upload.failure_attempt_count)
    );
    bind_integer(
        database,
        statement.get(),
        16,
        upload.directory ? 1 : 0
    );
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending upload: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_pending_upload(
    const std::string& drive_id,
    const std::string& remote_path
) {
    impl_->invoke([this, drive_id, remote_path] {
        remove_pending_upload_on_worker(drive_id, remote_path);
    });
}

void ItemDatabase::remove_pending_upload_on_worker(
    const std::string& drive_id,
    const std::string& remote_path
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty() || remote_path.empty()) {
        throw std::invalid_argument(
            "pending upload removal requires drive and remote paths"
        );
    }
    Statement statement{
        database,
        "DELETE FROM pending_upload WHERE drive_id = ?1 AND remote_path = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_path);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending upload: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingUpload> ItemDatabase::pending_uploads(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_uploads_on_worker(drive_id);
    });
}

std::vector<PendingUpload> ItemDatabase::pending_uploads_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_path, local_path, snapshot_path, "
        "content_fingerprint, local_size, local_modified_ticks, remote_id, "
        "expected_etag, upload_url, upload_expiration, completed_bytes, "
        "failure_code, failure_message, failure_attempt_count, directory "
        "FROM pending_upload WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingUpload> uploads;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending uploads: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        auto remote_id = column_text(statement.get(), 7);
        uploads.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_path = column_text(statement.get(), 1),
            .local_path = column_text(statement.get(), 2),
            .snapshot_path = column_text(statement.get(), 3),
            .content_fingerprint = column_text(statement.get(), 4),
            .local_size = sqlite3_column_int64(statement.get(), 5),
            .local_modified_ticks = sqlite3_column_int64(statement.get(), 6),
            .remote_id = remote_id.empty() ?
                std::nullopt :
                std::optional{std::move(remote_id)},
            .expected_etag = column_text(statement.get(), 8),
            .upload_url = column_text(statement.get(), 9),
            .upload_expiration = column_text(statement.get(), 10),
            .completed_bytes = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 11)
            ),
            .failure_code = column_text(statement.get(), 12),
            .failure_message = column_text(statement.get(), 13),
            .failure_attempt_count = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 14)
            ),
            .directory = sqlite3_column_int(statement.get(), 15) != 0,
        });
    }
    return uploads;
}

void ItemDatabase::commit_upload(
    const PendingUpload& upload,
    ItemState item
) {
    impl_->invoke([this, upload, item = std::move(item)] {
        commit_upload_on_worker(upload, item);
    });
}

void ItemDatabase::commit_upload_on_worker(
    const PendingUpload& upload,
    const ItemState& item
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (upload.drive_id != item.drive_id ||
        upload.remote_path != item.remote_path) {
        throw std::invalid_argument(
            "pending upload and item state do not identify the same item"
        );
    }
    Transaction transaction{database};
    upsert_on_worker(item);
    Statement statement{
        database,
        "DELETE FROM pending_upload WHERE drive_id = ?1 AND remote_path = ?2;"
    };
    bind_text(database, statement.get(), 1, upload.drive_id);
    bind_text(database, statement.get(), 2, upload.remote_path);
    if (sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
        throw std::runtime_error(
            "cannot complete pending upload journal: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();
}

}  // namespace onedrive::storage
