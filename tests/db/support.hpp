#pragma once

#include "onedrive/account/account_state.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::test::db {

[[nodiscard]] account::DriveIdentity identity();
[[nodiscard]] bool
execute_schema(const std::filesystem::path& path, const char* schema);
[[nodiscard]] bool
create_version_one_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_two_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_three_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_four_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_five_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_six_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_seven_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_eight_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_nine_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_ten_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_eleven_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twelve_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_thirteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_fourteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_fifteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_sixteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_seventeen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_eighteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_nineteen_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twenty_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twenty_one_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twenty_two_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twenty_three_database(const std::filesystem::path& path);
[[nodiscard]] bool
create_version_twenty_four_database(const std::filesystem::path& path);
[[nodiscard]] bool identity_row_is_valid(const std::filesystem::path& path);
[[nodiscard]] bool
schema_version_is(const std::filesystem::path& path, int expected);
[[nodiscard]] bool
create_current_database(const std::filesystem::path& directory);
[[nodiscard]] std::optional<std::string>
database_open_error(const std::filesystem::path& directory);

} // namespace onedrive::test::db
