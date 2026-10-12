#include "onedrive/storage/item_database.hpp"
#include "storage/file_hash.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

#include <stdexcept>
#include <utility>

namespace onedrive::storage {

using item_database_detail::bind_integer;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::parse_file_hash;
using item_database_detail::serialize_file_hash;
using item_database_detail::Statement;
using item_database_detail::Transaction;

void ItemDatabase::save_pending_delta(PendingDelta delta) {
    impl_->invoke([this, delta = std::move(delta)] {
        save_pending_delta_on_worker(delta);
    });
}

void ItemDatabase::save_pending_delta_on_worker(const PendingDelta& delta) {
    auto* database = impl_->database.get();
    if (!database) {
        throw std::runtime_error{"state database is not open"};
    }
    if (delta.drive_id.empty() || delta.scope_fingerprint.empty() ||
        delta.delta.delta_link.empty() ||
        (delta.apply_mode != DeltaApplyMode::merge &&
         delta.apply_mode != DeltaApplyMode::replace)) {
        throw std::invalid_argument{
            "pending delta requires a valid scope and cursor"
        };
    }
    Transaction transaction{database};
    remove_pending_delta_on_worker(delta.drive_id);
    Statement header{
        database,
        "INSERT INTO pending_delta (drive_id, scope_fingerprint,"
        "sync_filter_fingerprint, applied_delta_link, "
        "applied_filter_fingerprint,"
        "delta_link, apply_mode) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);"
    };
    bind_text(database, header.get(), 1, delta.drive_id);
    bind_text(database, header.get(), 2, delta.scope_fingerprint);
    bind_text(database, header.get(), 3, delta.sync_filter_fingerprint);
    if (delta.applied_delta_link) {
        bind_text(database, header.get(), 4, *delta.applied_delta_link);
    }
    if (delta.applied_filter_fingerprint) {
        bind_text(database, header.get(), 5, *delta.applied_filter_fingerprint);
    }
    bind_text(database, header.get(), 6, delta.delta.delta_link);
    bind_integer(
        database, header.get(), 7, delta.apply_mode == DeltaApplyMode::replace
    );
    header.execute("cannot save pending delta cursor");
    Statement change{
        database,
        "INSERT INTO pending_delta_change (drive_id, remote_id, position,"
        "name, etag, ctag, parent_id, remote_path, last_modified, size,"
        "directory, deleted, root, malware, content_hash_algorithm,"
        "content_hash_value, validate_content)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,"
        "?11, ?12, ?13, ?14, ?15, ?16, ?17);"
    };
    std::int64_t position = 0;
    for (const auto& item : delta.delta.changes) {
        if (item.id.empty() || item.size < 0 ||
            (item.content_hash && item.content_hash->value.empty())) {
            throw std::invalid_argument{
                "pending delta contains invalid remote metadata"
            };
        }
        bind_text(database, change.get(), 1, delta.drive_id);
        bind_text(database, change.get(), 2, item.id);
        bind_integer(database, change.get(), 3, position++);
        bind_text(database, change.get(), 4, item.name);
        bind_text(database, change.get(), 5, item.etag);
        bind_text(database, change.get(), 6, item.ctag);
        bind_text(database, change.get(), 7, item.parent_id);
        bind_text(database, change.get(), 8, item.remote_path);
        bind_text(database, change.get(), 9, item.last_modified);
        bind_integer(database, change.get(), 10, item.size);
        bind_integer(database, change.get(), 11, item.directory);
        bind_integer(database, change.get(), 12, item.deleted);
        bind_integer(database, change.get(), 13, item.root);
        bind_integer(database, change.get(), 14, item.malware);
        const auto [algorithm, value] = serialize_file_hash(item.content_hash);
        bind_text(database, change.get(), 15, algorithm);
        bind_text(database, change.get(), 16, value);
        bind_integer(database, change.get(), 17, item.validate_content);
        change.execute("cannot save pending remote change");
        change.reset();
    }
    transaction.commit();
}

std::optional<PendingDelta>
ItemDatabase::pending_delta(const std::string& drive_id) const {
    return impl_->invoke([this, drive_id] {
        return pending_delta_on_worker(drive_id);
    });
}

std::optional<PendingDelta>
ItemDatabase::pending_delta_on_worker(const std::string& drive_id) const {
    auto* database = impl_->database.get();
    if (!database) {
        throw std::runtime_error{"state database is not open"};
    }
    Statement header{
        database,
        "SELECT scope_fingerprint, sync_filter_fingerprint, applied_delta_link,"
        "applied_filter_fingerprint, delta_link, apply_mode"
        " FROM pending_delta WHERE drive_id = ?1;"
    };
    bind_text(database, header.get(), 1, drive_id);
    if (!header.next_row("cannot read pending delta cursor")) {
        return std::nullopt;
    }
    PendingDelta result{
        .drive_id = drive_id,
        .scope_fingerprint = column_text(header.get(), 0),
        .sync_filter_fingerprint = column_text(header.get(), 1),
        .applied_delta_link = {},
        .applied_filter_fingerprint = {},
        .delta = {.changes = {}, .delta_link = column_text(header.get(), 4)},
        .apply_mode = sqlite3_column_int(header.get(), 5) == 1
                          ? DeltaApplyMode::replace
                          : DeltaApplyMode::merge,
    };
    if (sqlite3_column_type(header.get(), 2) != SQLITE_NULL) {
        result.applied_delta_link = column_text(header.get(), 2);
    }
    if (sqlite3_column_type(header.get(), 3) != SQLITE_NULL) {
        result.applied_filter_fingerprint = column_text(header.get(), 3);
    }
    Statement changes{
        database,
        "SELECT remote_id, name, etag, ctag, parent_id, remote_path,"
        "last_modified, size, directory, deleted, root, malware,"
        "content_hash_algorithm, content_hash_value, validate_content"
        " FROM pending_delta_change WHERE drive_id = ?1 ORDER BY position;"
    };
    bind_text(database, changes.get(), 1, drive_id);
    while (changes.next_row("cannot read pending remote changes")) {
        result.delta.changes.push_back({
            .id = column_text(changes.get(), 0),
            .name = column_text(changes.get(), 1),
            .etag = column_text(changes.get(), 2),
            .ctag = column_text(changes.get(), 3),
            .parent_id = column_text(changes.get(), 4),
            .remote_path = column_text(changes.get(), 5),
            .last_modified = column_text(changes.get(), 6),
            .size = sqlite3_column_int64(changes.get(), 7),
            .directory = sqlite3_column_int(changes.get(), 8) != 0,
            .deleted = sqlite3_column_int(changes.get(), 9) != 0,
            .root = sqlite3_column_int(changes.get(), 10) != 0,
            .malware = sqlite3_column_int(changes.get(), 11) != 0,
            .content_hash = parse_file_hash(
                column_text(changes.get(), 12),
                column_text(changes.get(), 13),
                "pending remote change"
            ),
            .validate_content = sqlite3_column_int(changes.get(), 14) != 0,
        });
    }
    return result;
}

void ItemDatabase::remove_pending_delta(const std::string& drive_id) {
    impl_->invoke([this, drive_id] {
        remove_pending_delta_on_worker(drive_id);
    });
}

void ItemDatabase::remove_pending_delta_on_worker(const std::string& drive_id) {
    auto* database = impl_->database.get();
    if (!database) {
        throw std::runtime_error{"state database is not open"};
    }
    Statement statement{
        database, "DELETE FROM pending_delta WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    statement.execute("cannot clear pending delta snapshot");
}

} // namespace onedrive::storage
