#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace onedrive::storage::item_database_detail {

class DatabaseCorruption final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void verify_database_integrity(sqlite3* database);
[[nodiscard]] std::string full_integrity_result(sqlite3* database);
void configure_writable_database(sqlite3* database);
[[nodiscard]] std::filesystem::path quarantine_corrupt_database(
    const std::filesystem::path& database_path
);
void verify_current_schema(sqlite3* database);
void migrate_schema(sqlite3* database);

}  // namespace onedrive::storage::item_database_detail
