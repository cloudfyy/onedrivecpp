#include "onedrive/storage/item_database.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::storage {

using item_database_detail::bind_integer;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::Statement;
using item_database_detail::Transaction;

namespace {

PartialDownload partial_download_from_row(sqlite3_stmt* statement) {
    const auto completed = sqlite3_column_int64(statement, 12);
    if (completed < 0) {
        throw std::runtime_error(
            "partial download contains a negative completed byte count"
        );
    }
    return {
        .item =
            {
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
                .local_size = 0,
                .local_modified_ticks = 0,
                .directory = sqlite3_column_int(statement, 10) != 0,
            },
        .temporary_path = column_text(statement, 11),
        .completed_bytes = static_cast<std::uint64_t>(completed),
    };
}

} // namespace

void ItemDatabase::save_pending_download(PendingDownload download) {
    impl_->invoke([this, download = std::move(download)] {
        save_pending_download_on_worker(download);
    });
}

void ItemDatabase::save_pending_download_on_worker(
    const PendingDownload& download
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (download.item.drive_id.empty() || download.item.remote_id.empty() ||
        download.temporary_path.empty() ||
        download.content_fingerprint.empty()) {
        throw std::invalid_argument(
            "pending download requires drive, item, temporary path, and "
            "fingerprint"
        );
    }
    if (download.backup_path.empty() != download.backup_fingerprint.empty()) {
        throw std::invalid_argument(
            "pending download safeBackup path and fingerprint must be "
            "provided together"
        );
    }

    Statement statement{
        database,
        "INSERT INTO pending_download ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "content_fingerprint, backup_path, backup_fingerprint"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15"
        ") "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, ctag = excluded.ctag, "
        "remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, temporary_path = "
        "excluded.temporary_path, "
        "content_fingerprint = excluded.content_fingerprint, "
        "backup_path = excluded.backup_path, "
        "backup_fingerprint = excluded.backup_fingerprint;"
    };
    bind_text(database, statement.get(), 1, download.item.drive_id);
    bind_text(database, statement.get(), 2, download.item.remote_id);
    bind_text(database, statement.get(), 3, download.item.parent_id);
    bind_text(database, statement.get(), 4, download.item.name);
    bind_text(database, statement.get(), 5, download.item.etag);
    bind_text(database, statement.get(), 6, download.item.ctag);
    bind_text(database, statement.get(), 7, download.item.remote_path);
    bind_text(database, statement.get(), 8, download.item.local_path.string());
    bind_text(database, statement.get(), 9, download.item.last_modified);
    bind_integer(database, statement.get(), 10, download.item.size);
    bind_integer(
        database, statement.get(), 11, download.item.directory ? 1 : 0
    );
    bind_text(database, statement.get(), 12, download.temporary_path.string());
    bind_text(database, statement.get(), 13, download.content_fingerprint);
    bind_text(database, statement.get(), 14, download.backup_path.string());
    bind_text(database, statement.get(), 15, download.backup_fingerprint);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot save pending download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    spdlog::debug(
        "Saved pending download journal for '{}'", download.item.remote_path
    );
}

void ItemDatabase::remove_pending_download(
    const std::string& drive_id, const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_download_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_download_on_worker(
    const std::string& drive_id, const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_download WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    spdlog::debug(
        "Removed pending download journal for drive '{}', item '{}'",
        drive_id,
        remote_id
    );
}

std::vector<PendingDownload>
ItemDatabase::pending_downloads(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return pending_downloads_on_worker(drive_id);
    });
}

std::vector<PendingDownload>
ItemDatabase::pending_downloads_on_worker(const std::string& drive_id) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "content_fingerprint, backup_path, backup_fingerprint "
        "FROM pending_download WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingDownload> downloads;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending downloads: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        downloads.push_back({
            .item =
                {
                    .drive_id = column_text(statement.get(), 0),
                    .remote_id = column_text(statement.get(), 1),
                    .parent_id = column_text(statement.get(), 2),
                    .name = column_text(statement.get(), 3),
                    .etag = column_text(statement.get(), 4),
                    .ctag = column_text(statement.get(), 5),
                    .remote_path = column_text(statement.get(), 6),
                    .local_path = column_text(statement.get(), 7),
                    .last_modified = column_text(statement.get(), 8),
                    .size = sqlite3_column_int64(statement.get(), 9),
                    .local_size = 0,
                    .local_modified_ticks = 0,
                    .directory = sqlite3_column_int(statement.get(), 10) != 0,
                },
            .temporary_path = column_text(statement.get(), 11),
            .content_fingerprint = column_text(statement.get(), 12),
            .backup_path = column_text(statement.get(), 13),
            .backup_fingerprint = column_text(statement.get(), 14),
        });
    }
    return downloads;
}

void ItemDatabase::save_partial_download(PartialDownload download) {
    impl_->invoke([this, download = std::move(download)] {
        save_partial_download_on_worker(download);
    });
}

void ItemDatabase::save_partial_download_on_worker(
    const PartialDownload& download
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (download.item.drive_id.empty() || download.item.remote_id.empty() ||
        download.temporary_path.empty() || download.item.size < 0 ||
        download.completed_bytes >
            static_cast<std::uint64_t>(download.item.size) ||
        download.completed_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()
            )) {
        throw std::invalid_argument(
            "partial download requires valid item, path, size, and offset"
        );
    }

    Statement statement{
        database,
        "INSERT INTO partial_download ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "completed_bytes"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, ctag = excluded.ctag, "
        "remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, "
        "temporary_path = excluded.temporary_path, "
        "completed_bytes = excluded.completed_bytes, "
        "updated_at = unixepoch();"
    };
    bind_text(database, statement.get(), 1, download.item.drive_id);
    bind_text(database, statement.get(), 2, download.item.remote_id);
    bind_text(database, statement.get(), 3, download.item.parent_id);
    bind_text(database, statement.get(), 4, download.item.name);
    bind_text(database, statement.get(), 5, download.item.etag);
    bind_text(database, statement.get(), 6, download.item.ctag);
    bind_text(database, statement.get(), 7, download.item.remote_path);
    bind_text(database, statement.get(), 8, download.item.local_path.string());
    bind_text(database, statement.get(), 9, download.item.last_modified);
    bind_integer(database, statement.get(), 10, download.item.size);
    bind_integer(
        database, statement.get(), 11, download.item.directory ? 1 : 0
    );
    bind_text(database, statement.get(), 12, download.temporary_path.string());
    bind_integer(
        database,
        statement.get(),
        13,
        static_cast<std::int64_t>(download.completed_bytes)
    );
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot save partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_partial_download(
    const std::string& drive_id, const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_partial_download_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_partial_download_on_worker(
    const std::string& drive_id, const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM partial_download WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::optional<PartialDownload> ItemDatabase::partial_download(
    const std::string& drive_id, const std::string& remote_id
) const {
    return impl_->invoke([this, drive_id, remote_id] {
        return partial_download_on_worker(drive_id, remote_id);
    });
}

std::optional<PartialDownload> ItemDatabase::partial_download_on_worker(
    const std::string& drive_id, const std::string& remote_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "completed_bytes FROM partial_download "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return partial_download_from_row(statement.get());
}

std::vector<PartialDownload>
ItemDatabase::partial_downloads(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return partial_downloads_on_worker(drive_id);
    });
}

std::vector<PartialDownload>
ItemDatabase::partial_downloads_on_worker(const std::string& drive_id) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "completed_bytes FROM partial_download "
        "WHERE drive_id = ?1 ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PartialDownload> downloads;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read partial downloads: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        downloads.push_back(partial_download_from_row(statement.get()));
    }
    return downloads;
}

} // namespace onedrive::storage
