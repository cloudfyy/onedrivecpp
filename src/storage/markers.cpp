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
