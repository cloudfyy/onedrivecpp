#include "onedrive/storage/item_database.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/path_security.hpp"
#include "storage/query.hpp"
#include "storage/schema.hpp"
#include "storage/schema_internal.hpp"
#include "storage/sqlite.hpp"
#include "storage/worker.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
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
using item_database_detail::execute_sql;
using item_database_detail::full_integrity_result;
using item_database_detail::migrate_schema;
using item_database_detail::query_count;
using item_database_detail::quarantine_corrupt_database;
using item_database_detail::require_pragma_value;
using item_database_detail::verify_current_schema;
using item_database_detail::verify_database_integrity;

constexpr mode_t private_directory_mode = S_IRWXU;
constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr int database_busy_timeout_milliseconds = 5000;

enum class DatabaseAccess {
    read_only,
    writable,
};

enum class FileCreation {
    existing_only,
    create_if_missing,
};

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

void activate_database_pragmas(
    sqlite3* database,
    DatabaseAccess access
) {
    execute_sql(database, "PRAGMA trusted_schema = OFF;");
    execute_sql(database, "PRAGMA foreign_keys = ON;");
    require_pragma_value(
        database, "PRAGMA trusted_schema;", "0", "trusted schema"
    );
    require_pragma_value(
        database, "PRAGMA foreign_keys;", "1", "foreign-key enforcement"
    );
    if (access == DatabaseAccess::read_only) {
        execute_sql(database, "PRAGMA query_only = ON;");
        require_pragma_value(
            database, "PRAGMA query_only;", "1", "read-only diagnostics"
        );
    }
}

void secure_directory(
    const std::filesystem::path& path, std::string_view description
) {
    onedrive::util::UniqueFD descriptor{
        onedrive::util::open_path_no_symlinks(
            path, O_RDONLY | O_DIRECTORY
        )
    };
    static_cast<void>(onedrive::util::secure_owned_directory(
        descriptor.get(),
        path,
        private_directory_mode,
        description
    ));
}

void secure_database_file(
    const std::filesystem::path& path,
    FileCreation creation
) {
    const bool create = creation == FileCreation::create_if_missing;
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
    static_cast<void>(onedrive::util::secure_owned_regular_file(
        descriptor.get(),
        path,
        private_file_mode,
        "SQLite state file"
    ));
}

void validate_diagnostic_file(const std::filesystem::path& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) == -1) {
        throw std::runtime_error(
            "cannot inspect database security: " +
            std::string{onedrive::util::system_error_message(errno)}
        );
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error(
            "database path is not a regular non-symbolic-link file"
        );
    }
    onedrive::util::reject_symlink_components(path, "state database");
    if (status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & 07777) != private_file_mode) {
        throw std::runtime_error(
            "database must be a single-link, current-user-owned mode 0600 file"
        );
    }
}

SqliteHandle open_maintenance_database(
    const std::filesystem::path& path, DatabaseAccess access
) {
    validate_diagnostic_file(path);
    for (const auto* suffix : {"-wal", "-shm"}) {
        const std::filesystem::path sidecar{path.string() + suffix};
        if (std::filesystem::exists(std::filesystem::symlink_status(sidecar))) {
            validate_diagnostic_file(sidecar);
        }
    }
    sqlite3* handle = nullptr;
    const int flags = access == DatabaseAccess::read_only
                          ? SQLITE_OPEN_READONLY
                          : SQLITE_OPEN_READWRITE;
    const int result = sqlite3_open_v2(
        path.c_str(), &handle, flags | SQLITE_OPEN_NOFOLLOW, nullptr
    );
    SqliteHandle database{handle};
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot open state database: " +
            std::string{handle ? sqlite3_errmsg(handle) : "SQLite open failed"}
        );
    }
    configure_database_connection(handle);
    activate_database_pragmas(handle, access);
    return database;
}

std::filesystem::path backup_database(
    sqlite3* source, const std::filesystem::path& path, int version
) {
    auto name =
        path.string() + ".pre-migrate-v" + std::to_string(version) + "-XXXXXX";
    util::UniqueFD file{::mkstemp(name.data())};
    if (!file) {
        throw std::runtime_error(
            "cannot create migration backup: " +
            std::string{util::system_error_message(errno)}
        );
    }
    const std::filesystem::path backup_path{name};
    {
        auto destination =
            open_maintenance_database(backup_path, DatabaseAccess::writable);
        sqlite3_backup* backup =
            sqlite3_backup_init(destination.get(), "main", source, "main");
        if (backup == nullptr) {
            throw std::runtime_error(
                "cannot initialize migration backup '" + name +
                "': " + sqlite3_errmsg(destination.get())
            );
        }
        const int copied = sqlite3_backup_step(backup, -1);
        const int finished = sqlite3_backup_finish(backup);
        if (copied != SQLITE_DONE || finished != SQLITE_OK) {
            throw std::runtime_error(
                "cannot complete migration backup '" + name +
                "': " + sqlite3_errmsg(destination.get())
            );
        }
        execute_sql(destination.get(), "PRAGMA journal_mode = DELETE;");
        const auto integrity = full_integrity_result(destination.get());
        if (integrity != "ok") {
            throw std::runtime_error(
                "migration backup failed integrity check: " + integrity
            );
        }
    }
    util::UniqueFD directory{
        util::open_path_no_symlinks(path.parent_path(), O_RDONLY | O_DIRECTORY)
    };
    if (::fsync(file.get()) == -1 || ::fsync(directory.get()) == -1) {
        throw std::runtime_error(
            "cannot flush migration backup '" + name +
            "': " + util::system_error_message(errno)
        );
    }
    return backup_path;
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
            .migration_required = false,
            .schema_version = std::nullopt,
            .latest_schema_version =
                item_database_detail::current_schema_version,
            .detail = {},
        };
        try {
            auto database = open_maintenance_database(
                entry.path(), DatabaseAccess::read_only
            );
            result.schema_version =
                item_database_detail::schema_version(database.get());
            result.detail = full_integrity_result(database.get());
            if (result.detail == "ok") {
                if (*result.schema_version > 0 &&
                    *result.schema_version < result.latest_schema_version) {
                    result.migration_required = true;
                    result.detail =
                        "Database upgrade required; run 'state migrate "
                        "--dry-run' to preview.";
                } else if (*result.schema_version !=
                           result.latest_schema_version) {
                    result.detail =
                        "Unsupported state database schema version " +
                        std::to_string(*result.schema_version);
                } else {
                    verify_current_schema(database.get());
                    result.healthy = true;
                }
            }
        } catch (const std::exception& error) {
            result.detail = error.what();
        }
        results.push_back(std::move(result));
    }
    std::ranges::sort(results, {}, &DatabaseIntegrityResult::path);
    return results;
}

std::filesystem::path
migrate_state_database(const std::filesystem::path& database_path) {
    auto database =
        open_maintenance_database(database_path, DatabaseAccess::writable);
    const auto version = item_database_detail::schema_version(database.get());
    if (version <= 0 ||
        version > item_database_detail::current_schema_version) {
        throw std::runtime_error(
            "unsupported state database schema version " +
            std::to_string(version)
        );
    }
    const auto integrity = full_integrity_result(database.get());
    if (integrity != "ok") {
        throw std::runtime_error(
            "state database integrity check failed: " + integrity
        );
    }
    if (version == item_database_detail::current_schema_version) {
        verify_current_schema(database.get());
        return {};
    }
    const auto backup = backup_database(database.get(), database_path, version);
    try {
        migrate_schema(database.get());
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "database migration failed; backup retained at '" +
            backup.string() + "': " + error.what()
        );
    }
    return backup;
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

void ItemDatabase::open_read_only() {
    impl_->invoke([this] {
        open_read_only_on_worker();
    });
}

void ItemDatabase::open_read_only_on_worker() {
    spdlog::debug("Opening synchronization state database read-only");
    if (identity_.user_id.empty() || identity_.drive_id.empty()) {
        throw std::invalid_argument(
            "read-only state database requires an account and drive identity"
        );
    }
    onedrive::util::reject_symlink_components(
        state_directory_, "SQLite state directory"
    );
    impl_->database.reset();

    const auto database_path = state_directory_ / "items.sqlite3";
    sqlite3* database = nullptr;
    const int result = sqlite3_open_v2(
        database_path.string().c_str(),
        &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
        nullptr
    );
    SqliteHandle opened_database{database};
    if (result != SQLITE_OK) {
        const std::string message =
            opened_database == nullptr ?
                "unknown SQLite error" :
                sqlite3_errmsg(opened_database.get());
        throw std::runtime_error(
            "cannot open state database read-only '" +
            database_path.string() + "': " + message
        );
    }
    impl_->database = std::move(opened_database);
    try {
        configure_database_connection(database);
        activate_database_pragmas(database, DatabaseAccess::read_only);
        verify_database_integrity(database);
        verify_current_schema(database);
    } catch (const std::exception& error) {
        impl_->database.reset();
        throw std::runtime_error(
            "cannot safely inspect state database '" +
            database_path.string() + "': " + error.what()
        );
    }

    Statement identity_query{
        database,
        "SELECT user_id, drive_id FROM identity WHERE singleton = 1;"
    };
    const int identity_result = sqlite3_step(identity_query.get());
    if (identity_result != SQLITE_ROW) {
        const std::string detail =
            identity_result == SQLITE_DONE ?
                "database identity is missing" :
                sqlite3_errmsg(database);
        impl_->database.reset();
        throw std::runtime_error(
            "cannot validate state database identity: " + detail
        );
    }
    if (column_text(identity_query.get(), 0) != identity_.user_id ||
        column_text(identity_query.get(), 1) != identity_.drive_id) {
        impl_->database.reset();
        throw std::runtime_error(
            "state database identity does not match the current Microsoft "
            "account and drive"
        );
    }
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
    secure_database_file(database_path, FileCreation::create_if_missing);
    secure_database_file(
        database_path.string() + "-wal", FileCreation::existing_only
    );
    secure_database_file(
        database_path.string() + "-shm", FileCreation::existing_only
    );
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
        activate_database_pragmas(database, DatabaseAccess::writable);
        configure_writable_database(database);
        migrate_schema(database);
        verify_database_integrity(database);
        verify_current_schema(database);
        secure_database_file(database_path, FileCreation::existing_only);
        secure_database_file(
            database_path.string() + "-wal", FileCreation::existing_only
        );
        secure_database_file(
            database_path.string() + "-shm", FileCreation::existing_only
        );
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
    identity_upsert.execute("cannot update state database identity");
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
    mapping_upsert.execute("cannot update configured Drive ID mapping");

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

}  // namespace onedrive::storage
