#include "onedrive/storage/item_database.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/path_security.hpp"
#include "storage/database_worker.hpp"
#include "storage/schema.hpp"
#include "storage/sqlite_support.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace onedrive::storage {
namespace {

using item_database_detail::DatabaseCorruption;
using item_database_detail::SqliteHandle;
using item_database_detail::Statement;
using item_database_detail::Transaction;
using item_database_detail::bind_blob;
using item_database_detail::bind_integer;
using item_database_detail::bind_text;
using item_database_detail::column_text;
using item_database_detail::configure_writable_database;
using item_database_detail::execute;
using item_database_detail::full_integrity_result;
using item_database_detail::migrate_schema;
using item_database_detail::quarantine_corrupt_database;
using item_database_detail::require_pragma_value;
using item_database_detail::verify_current_schema;
using item_database_detail::verify_database_integrity;

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr int database_busy_timeout_milliseconds = 5000;

void require_sqlite_result(
    sqlite3* database, int result, std::string_view operation
) {
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            std::string{operation} + ": " + sqlite3_errmsg(database)
        );
    }
}

void configure_database_connection(sqlite3* database) {
    require_sqlite_result(
        database,
        sqlite3_extended_result_codes(database, 1),
        "cannot enable extended SQLite result codes"
    );
    require_sqlite_result(
        database,
        sqlite3_busy_timeout(database, database_busy_timeout_milliseconds),
        "cannot configure SQLite busy timeout"
    );
    require_sqlite_result(
        database,
        sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr),
        "cannot enable SQLite defensive mode"
    );
    require_sqlite_result(
        database,
        sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr),
        "cannot disable trusted SQLite schema"
    );
    require_sqlite_result(
        database,
        sqlite3_db_config(
            database, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr
        ),
        "cannot disable SQLite extension loading"
    );

    sqlite3_limit(database, SQLITE_LIMIT_LENGTH, 16 * 1024 * 1024);
    sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, 1024 * 1024);
    sqlite3_limit(database, SQLITE_LIMIT_COLUMN, 128);
    sqlite3_limit(database, SQLITE_LIMIT_COMPOUND_SELECT, 32);
    sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_limit(database, SQLITE_LIMIT_VARIABLE_NUMBER, 128);
}

void activate_database_pragmas(sqlite3* database, bool writable) {
    execute(database, "PRAGMA trusted_schema = OFF;");
    execute(database, "PRAGMA foreign_keys = ON;");
    require_pragma_value(
        database, "PRAGMA trusted_schema;", "0", "trusted schema"
    );
    require_pragma_value(
        database, "PRAGMA foreign_keys;", "1", "foreign-key enforcement"
    );
    if (!writable) {
        execute(database, "PRAGMA query_only = ON;");
        require_pragma_value(
            database, "PRAGMA query_only;", "1", "read-only diagnostics"
        );
    }
}

void secure_directory(
    const std::filesystem::path& path, std::string_view description
) {
    onedrive::util::reject_symlink_components(path, description);
    onedrive::util::UniqueFD descriptor{
        ::open(
            path.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open " + std::string{description} + " '" + path.string() +
            "': " + std::strerror(errno)
        );
    }
    struct stat status{};
    if (::fstat(descriptor.get(), &status) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot inspect " + std::string{description} + " '" +
            path.string() + "': " + message
        );
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()) {
        throw std::runtime_error(
            std::string{description} +
            " must be a directory owned by the current user: " + path.string()
        );
    }
    if ((status.st_mode & 07777) != private_directory_mode &&
        ::fchmod(descriptor.get(), private_directory_mode) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot secure " + std::string{description} + " '" + path.string() +
            "': " + message
        );
    }
}

void secure_database_file(const std::filesystem::path& path, bool create) {
    const int flags = O_RDWR | (create ? O_CREAT : 0);
    onedrive::util::UniqueFD descriptor;
    try {
        descriptor = onedrive::util::open_path_no_symlinks(
            path, flags, create ? private_file_mode : 0
        );
    } catch (const std::system_error& error) {
        if (!create && error.code().value() == ENOENT) {
            return;
        }
        throw;
    }
    struct stat status{};
    if (::fstat(descriptor.get(), &status) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot inspect SQLite state file '" + path.string() +
            "': " + message
        );
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1) {
        throw std::runtime_error(
            "SQLite state file must be a regular, single-link file owned by "
            "the current user: " +
            path.string()
        );
    }
    if ((status.st_mode & 07777) != private_file_mode &&
        ::fchmod(descriptor.get(), private_file_mode) == -1) {
        const std::string message = std::strerror(errno);
        throw std::runtime_error(
            "cannot secure SQLite state file '" + path.string() +
            "': " + message
        );
    }
}

std::size_t query_count(
    sqlite3* database,
    const char* sql,
    const std::string* value = nullptr
) {
    Statement statement{database, sql};
    if (value != nullptr) {
        bind_text(database, statement.get(), 1, *value);
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot count synchronization state rows: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const auto count = sqlite3_column_int64(statement.get(), 0);
    if (count < 0) {
        throw std::runtime_error("SQLite returned a negative row count");
    }
    return static_cast<std::size_t>(count);
}

}  // namespace

std::vector<DatabaseIntegrityResult>
diagnose_state_databases(const std::filesystem::path& state_directory) {
    if (!std::filesystem::exists(state_directory)) {
        return {};
    }
    onedrive::util::reject_symlink_components(
        state_directory, "state directory"
    );

    std::vector<DatabaseIntegrityResult> results;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{state_directory}) {
        if (entry.path().filename() != "items.sqlite3") {
            continue;
        }
        DatabaseIntegrityResult result{
            .path = entry.path(),
            .healthy = false,
            .detail = {},
        };
        const auto status = entry.symlink_status();
        if (std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            result.detail =
                "database path is not a regular non-symbolic-link file";
            results.push_back(std::move(result));
            continue;
        }
        struct stat file_status{};
        if (::lstat(entry.path().c_str(), &file_status) == -1) {
            result.detail = "cannot inspect database security: " +
                            std::string{std::strerror(errno)};
            results.push_back(std::move(result));
            continue;
        }
        if (file_status.st_uid != ::geteuid() || file_status.st_nlink != 1 ||
            (file_status.st_mode & 07777) != private_file_mode) {
            result.detail =
                "database must be a single-link, current-user-owned mode "
                "0600 file";
            results.push_back(std::move(result));
            continue;
        }

        sqlite3* handle = nullptr;
        const int open_result = sqlite3_open_v2(
            entry.path().string().c_str(),
            &handle,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
            nullptr
        );
        SqliteHandle database{handle};
        if (open_result != SQLITE_OK) {
            result.detail = handle == nullptr ? "unknown SQLite open error"
                                              : sqlite3_errmsg(handle);
            results.push_back(std::move(result));
            continue;
        }

        try {
            configure_database_connection(database.get());
            activate_database_pragmas(database.get(), false);
            result.detail = full_integrity_result(database.get());
            if (result.detail == "ok") {
                verify_current_schema(database.get());
                result.healthy = true;
            }
        } catch (const std::exception& error) {
            result.detail = error.what();
        }
        results.push_back(std::move(result));
    }
    std::ranges::sort(results, {}, &DatabaseIntegrityResult::path);
    return results;
}

ItemDatabase::ItemDatabase(
    std::filesystem::path state_directory,
    account::DriveIdentity identity
)
    : state_directory_{std::move(state_directory)},
      identity_{std::move(identity)},
      impl_{std::make_unique<Impl>()} {}

ItemDatabase::~ItemDatabase() = default;

void ItemDatabase::open() {
    impl_->invoke([this] {
        open_on_worker(CorruptionRecovery::quarantine_and_rebuild);
    });
}

void ItemDatabase::open_on_worker(CorruptionRecovery recovery) {
    spdlog::debug("Opening synchronization state database");
    if (identity_.user_id.empty() || identity_.user_display_name.empty() ||
        identity_.configured_drive_id.empty() || identity_.drive_id.empty() ||
        identity_.drive_name.empty()) {
        throw std::invalid_argument(
            "state database requires a complete account and drive identity"
        );
    }
    onedrive::util::reject_symlink_components(
        state_directory_, "SQLite state directory"
    );
    std::filesystem::create_directories(state_directory_);
    secure_directory(state_directory_, "SQLite state directory");
    impl_->database.reset();

    sqlite3* database = nullptr;
    const auto database_path = state_directory_ / "items.sqlite3";
    secure_database_file(database_path, true);
    secure_database_file(database_path.string() + "-wal", false);
    secure_database_file(database_path.string() + "-shm", false);
    const int result = sqlite3_open_v2(
        database_path.string().c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW,
        nullptr
    );
    SqliteHandle opened_database{database};
    if (result != SQLITE_OK) {
        const std::string message =
            opened_database == nullptr ?
                "unknown SQLite error" :
                sqlite3_errmsg(opened_database.get());
        throw std::runtime_error(
            "cannot open state database '" + database_path.string() + "': " + message
        );
    }
    impl_->database = std::move(opened_database);

    try {
        configure_database_connection(database);
        verify_database_integrity(database);
        activate_database_pragmas(database, true);
        configure_writable_database(database);
        migrate_schema(database);
        verify_database_integrity(database);
        verify_current_schema(database);
        secure_database_file(database_path, false);
        secure_database_file(database_path.string() + "-wal", false);
        secure_database_file(database_path.string() + "-shm", false);
    } catch (const DatabaseCorruption& error) {
        impl_->database.reset();
        if (recovery == CorruptionRecovery::fail) {
            throw std::runtime_error(error.what());
        }
        const auto quarantine_path = quarantine_corrupt_database(database_path);
        spdlog::warn(
            "Rebuilding synchronization state after corruption; quarantined "
            "evidence remains at '{}'",
            quarantine_path.string()
        );
        open_on_worker(CorruptionRecovery::fail);
        return;
    } catch (const std::exception& error) {
        impl_->database.reset();
        throw std::runtime_error(
            "cannot safely use state database '" + database_path.string() +
            "': " + error.what() + ". Move or remove this database file and "
            "run synchronization again to rebuild its reconstructible state; "
            "local files will be preserved and may require conflict resolution"
        );
    }

    Statement identity_query{
        database,
        "SELECT user_id, drive_id FROM identity WHERE singleton = 1;"
    };
    const int identity_result = sqlite3_step(identity_query.get());
    if (identity_result == SQLITE_ROW &&
        (column_text(identity_query.get(), 0) != identity_.user_id ||
         column_text(identity_query.get(), 1) != identity_.drive_id)) {
        throw std::runtime_error(
            "state database identity does not match the current Microsoft "
            "account and drive"
        );
    }
    if (identity_result != SQLITE_ROW && identity_result != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot read state database identity: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const std::string avatar_content_type =
        identity_.photo ? identity_.photo->content_type : "";
    const std::vector<std::uint8_t> avatar_bytes =
        identity_.photo ? identity_.photo->bytes : std::vector<std::uint8_t>{};
    Statement identity_upsert{
        database,
        "INSERT INTO identity ("
        "singleton, user_id, user_display_name, drive_id, drive_name, "
        "avatar_content_type, avatar_bytes"
        ") VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(singleton) DO UPDATE SET "
        "user_display_name = excluded.user_display_name, "
        "drive_name = excluded.drive_name, "
        "avatar_content_type = CASE WHEN length(excluded.avatar_bytes) > 0 "
        "THEN excluded.avatar_content_type ELSE identity.avatar_content_type END, "
        "avatar_bytes = CASE WHEN length(excluded.avatar_bytes) > 0 "
        "THEN excluded.avatar_bytes ELSE identity.avatar_bytes END;"
    };
    bind_text(database, identity_upsert.get(), 1, identity_.user_id);
    bind_text(
        database,
        identity_upsert.get(),
        2,
        identity_.user_display_name
    );
    bind_text(database, identity_upsert.get(), 3, identity_.drive_id);
    bind_text(database, identity_upsert.get(), 4, identity_.drive_name);
    bind_text(database, identity_upsert.get(), 5, avatar_content_type);
    bind_blob(database, identity_upsert.get(), 6, avatar_bytes);
    if (sqlite3_step(identity_upsert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update state database identity: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    Statement mapping_upsert{
        database,
        "INSERT INTO drive_mapping ("
        "configured_drive_id, canonical_drive_id"
        ") VALUES (?1, ?2) "
        "ON CONFLICT(configured_drive_id) DO UPDATE SET "
        "canonical_drive_id = excluded.canonical_drive_id, "
        "last_resolved = unixepoch();"
    };
    bind_text(
        database,
        mapping_upsert.get(),
        1,
        identity_.configured_drive_id
    );
    bind_text(database, mapping_upsert.get(), 2, identity_.drive_id);
    if (sqlite3_step(mapping_upsert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update configured Drive ID mapping: " +
            std::string{sqlite3_errmsg(database)}
        );
    }

    const auto item_count =
        query_count(database, "SELECT COUNT(*) FROM item;");
    const auto blocked_count =
        query_count(database, "SELECT COUNT(*) FROM blocked_item;");
    spdlog::info(
        "Synchronization state database ready with {} tracked items and {} "
        "blocked items",
        item_count,
        blocked_count
    );
}

void ItemDatabase::upsert(ItemState item) {
    impl_->invoke([this, item = std::move(item)] {
        upsert_on_worker(item);
    });
}

void ItemDatabase::upsert_on_worker(const ItemState& item) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database,
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13, ?14, ?15) "
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
        "directory = excluded.directory;"
    };
    const std::string local_path = item.local_path.string();
    bind_text(database, statement.get(), 1, item.drive_id);
    bind_text(database, statement.get(), 2, item.remote_id);
    bind_text(database, statement.get(), 3, item.parent_id);
    bind_text(database, statement.get(), 4, item.name);
    bind_text(database, statement.get(), 5, item.etag);
    bind_text(database, statement.get(), 6, item.ctag);
    bind_text(database, statement.get(), 7, item.remote_path);
    bind_text(database, statement.get(), 8, local_path);
    bind_text(database, statement.get(), 9, item.last_modified);
    bind_integer(database, statement.get(), 10, item.size);
    bind_integer(database, statement.get(), 11, item.local_size);
    bind_integer(database, statement.get(), 12, item.local_modified_ticks);
    bind_integer(
        database,
        statement.get(),
        13,
        static_cast<std::int64_t>(item.local_device)
    );
    bind_integer(
        database,
        statement.get(),
        14,
        static_cast<std::int64_t>(item.local_inode)
    );
    bind_integer(database, statement.get(), 15, item.directory ? 1 : 0);

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update state database: " + std::string{sqlite3_errmsg(database)}
        );
    }
    spdlog::trace(
        "Updated synchronization state for drive '{}', item '{}'",
        item.drive_id,
        item.remote_id
    );
}

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
        throw std::invalid_argument("delta state requires a drive ID and delta link");
    }

    Transaction transaction{database};
    if (delta.apply_mode == DeltaApplyMode::replace) {
        Statement replace_statement{
            database,
            "DELETE FROM item WHERE drive_id = ?1;"
        };
        bind_text(database, replace_statement.get(), 1, delta.drive_id);
        if (sqlite3_step(replace_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot replace drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        Statement replace_blocked_statement{
            database,
            "DELETE FROM blocked_item WHERE drive_id = ?1;"
        };
        bind_text(
            database,
            replace_blocked_statement.get(),
            1,
            delta.drive_id
        );
        if (sqlite3_step(replace_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot replace blocked drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
    }
    Statement upsert_statement{
        database,
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13, ?14, ?15) "
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
        "directory = excluded.directory;"
    };
    for (auto& item : delta.upserts) {
        if (item.remote_id.empty()) {
            throw std::invalid_argument("cannot persist a delta item without an ID");
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
            database,
            upsert_statement.get(),
            12,
            item.local_modified_ticks
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
        bind_integer(
            database,
            upsert_statement.get(),
            15,
            item.directory ? 1 : 0
        );
        if (sqlite3_step(upsert_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot apply delta item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(upsert_statement.get());
        sqlite3_clear_bindings(upsert_statement.get());
    }

    Statement complete_move_statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& item : delta.upserts) {
        bind_text(
            database,
            complete_move_statement.get(),
            1,
            delta.drive_id
        );
        bind_text(
            database,
            complete_move_statement.get(),
            2,
            item.remote_id
        );
        if (sqlite3_step(complete_move_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot complete pending move journal: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(complete_move_statement.get());
        sqlite3_clear_bindings(complete_move_statement.get());
    }
    for (const auto& remote_id : delta.removals) {
        bind_text(
            database,
            complete_move_statement.get(),
            1,
            delta.drive_id
        );
        bind_text(
            database,
            complete_move_statement.get(),
            2,
            remote_id
        );
        if (sqlite3_step(complete_move_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot discard removed pending move journal: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(complete_move_statement.get());
        sqlite3_clear_bindings(complete_move_statement.get());
    }

    Statement delete_statement{
        database,
        "DELETE FROM item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument("cannot remove a delta item without an ID");
        }
        bind_text(database, delete_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_statement.get(), 2, remote_id);
        if (sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot remove delta item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(delete_statement.get());
        sqlite3_clear_bindings(delete_statement.get());
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
        if (suppression.remote_id.empty() ||
            suppression.local_path.empty() ||
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
            database,
            suppression_statement.get(),
            1,
            suppression.drive_id
        );
        bind_text(
            database,
            suppression_statement.get(),
            2,
            suppression.remote_id
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
        if (sqlite3_step(suppression_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot persist upload suppression: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(suppression_statement.get());
        sqlite3_clear_bindings(suppression_statement.get());
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
        if (sqlite3_step(delete_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot remove blocked item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(delete_blocked_statement.get());
        sqlite3_clear_bindings(delete_blocked_statement.get());
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
        bind_text(database, upsert_blocked_statement.get(), 7, item.remote_path);
        bind_text(
            database,
            upsert_blocked_statement.get(),
            8,
            item.last_modified
        );
        bind_integer(database, upsert_blocked_statement.get(), 9, item.size);
        bind_integer(
            database,
            upsert_blocked_statement.get(),
            10,
            item.directory ? 1 : 0
        );
        bind_integer(
            database,
            upsert_blocked_statement.get(),
            11,
            item.deleted ? 1 : 0
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            12,
            item.reason_code
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            13,
            item.reason_message
        );
        std::string hash_algorithm;
        std::string hash_value;
        if (item.content_hash.has_value()) {
            hash_algorithm =
                item.content_hash->algorithm == util::FileHashAlgorithm::sha256 ?
                    "sha256" :
                    "quick_xor";
            hash_value = item.content_hash->value;
            if (hash_value.empty()) {
                throw std::invalid_argument(
                    "blocked item content hash cannot be empty"
                );
            }
        }
        bind_text(
            database,
            upsert_blocked_statement.get(),
            14,
            hash_algorithm
        );
        bind_text(database, upsert_blocked_statement.get(), 15, hash_value);
        if (sqlite3_step(upsert_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot persist blocked item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(upsert_blocked_statement.get());
        sqlite3_clear_bindings(upsert_blocked_statement.get());
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
        database,
        state_statement.get(),
        3,
        delta.sync_filter_fingerprint
    );
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update delta link: " + std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();

    spdlog::debug(
        "Committed remote delta for drive '{}': {} upserts, {} removals, {} "
        "blocked, inventory {}, delta cursor advanced, {} total items tracked",
        delta.drive_id,
        delta.upserts.size(),
        delta.removals.size(),
        delta.blocked_upserts.size(),
        delta.apply_mode == DeltaApplyMode::replace ?
            "replaced" :
            "updated",
        query_count(database, "SELECT COUNT(*) FROM item;")
    );
}

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

std::optional<std::string> ItemDatabase::delta_link(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return delta_link_on_worker(drive_id);
    });
}

std::optional<std::string> ItemDatabase::delta_link_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database,
        "SELECT delta_link FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return column_text(statement.get(), 0);
}

std::optional<std::string> ItemDatabase::sync_filter_fingerprint(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return sync_filter_fingerprint_on_worker(drive_id);
    });
}

std::optional<std::string>
ItemDatabase::sync_filter_fingerprint_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT sync_filter_fingerprint FROM drive_state "
        "WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read sync filter fingerprint: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return column_text(statement.get(), 0);
}

std::optional<ItemState> ItemDatabase::find(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    return impl_->invoke([this, drive_id, remote_id] {
        return find_on_worker(drive_id, remote_id);
    });
}

std::optional<ItemState> ItemDatabase::find_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, directory FROM item "
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
            "cannot read synchronization item: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return ItemState{
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
        .local_size = sqlite3_column_int64(statement.get(), 10),
        .local_modified_ticks = sqlite3_column_int64(statement.get(), 11),
        .local_device = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 12)
        ),
        .local_inode = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 13)
        ),
        .directory = sqlite3_column_int(statement.get(), 14) != 0,
    };
}

std::vector<ItemState> ItemDatabase::drive_items(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return drive_items_on_worker(drive_id);
    });
}

std::vector<ItemState> ItemDatabase::drive_items_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, ctag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "local_device, local_inode, directory FROM item "
        "WHERE drive_id = ?1 ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<ItemState> result;
    while (true) {
        const int step_result = sqlite3_step(statement.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read synchronization drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        result.push_back({
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
            .local_size = sqlite3_column_int64(statement.get(), 10),
            .local_modified_ticks =
                sqlite3_column_int64(statement.get(), 11),
            .local_device = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 12)
            ),
            .local_inode = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 13)
            ),
            .directory = sqlite3_column_int(statement.get(), 14) != 0,
        });
    }
    return result;
}

std::size_t ItemDatabase::size() const {
    return impl_->invoke([this] {
        return size_on_worker();
    });
}

std::size_t ItemDatabase::size_on_worker() const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    return query_count(database, "SELECT COUNT(*) FROM item;");
}

}  // namespace onedrive::storage
