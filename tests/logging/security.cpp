#include "onedrive/logging/logging.hpp"
#include "support/common.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

[[nodiscard]] bool private_regular_file(
    const std::filesystem::path& path
) {
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           status.st_nlink == 1 &&
           (status.st_mode & 0777) == 0600;
}

int test_level_parsing() {
    const std::array levels{
        std::pair{"TrAcE", spdlog::level::trace},
        std::pair{"DeBuG", spdlog::level::debug},
        std::pair{"INFO", spdlog::level::info},
        std::pair{"WaRn", spdlog::level::warn},
        std::pair{"ERROR", spdlog::level::err},
        std::pair{"CrItIcAl", spdlog::level::critical},
        std::pair{"OFF", spdlog::level::off},
    };
    for (const auto& [name, expected] : levels) {
        const onedrive::logging::Session session{{.level = name}};
        if (spdlog::get_level() != expected) {
            return fail("case-insensitive log level was parsed incorrectly");
        }
    }
    for (const std::string_view invalid : std::array<std::string_view, 7>{
             "", " INFO", "info ", "WARNing", "\xc4", "invalid",
             std::string_view{"info\0extra", 10},
         }) {
        try {
            const onedrive::logging::Session session{
                {.level = std::string{invalid}}
            };
            return fail("invalid log level was accepted");
        } catch (const std::invalid_argument& error) {
            if (std::string_view{error.what()}.find("invalid log level:") != 0) {
                return fail("invalid log level lost its diagnostic");
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_private_log_and_rotation() {
    TemporaryDirectory temporary;
    std::filesystem::permissions(
        temporary.path(), std::filesystem::perms::owner_all
    );
    const auto log = temporary.path() / "onedrive.log";
    {
        onedrive::logging::Session session{{
            .level = "info",
            .file = log,
        }};
        spdlog::info("private log fixture");
    }
    if (!private_regular_file(log) ||
        !onedrive::test::read_file(log).contains("private log fixture")) {
        return fail("file log was not written as a private regular file");
    }

    {
        onedrive::logging::Session session{{
            .level = "info",
            .file = log,
        }};
        spdlog::info("{}", std::string(std::size_t{5} * 1024U * 1024U, 'x'));
        spdlog::warn("rotate");
    }
    const auto rotated = temporary.path() / "onedrive.log.1";
    if (!private_regular_file(log) || !private_regular_file(rotated) ||
        !onedrive::test::read_file(log).contains("rotate")) {
        return fail("rotated logs did not retain private file permissions");
    }
    return EXIT_SUCCESS;
}

int test_rejects_unsafe_log_files() {
    TemporaryDirectory temporary;
    std::filesystem::permissions(
        temporary.path(), std::filesystem::perms::owner_all
    );
    const auto victim = temporary.path() / "victim";
    onedrive::test::write_file(victim, "unchanged");

    const auto symlink = temporary.path() / "symlink.log";
    std::filesystem::create_symlink(victim, symlink);
    try {
        onedrive::logging::Session session{{
            .file = symlink,
        }};
        return fail("symbolic-link log file was accepted");
    } catch (const std::exception&) {
    }
    if (onedrive::test::read_file(victim) != "unchanged") {
        return fail("symbolic-link log target was modified");
    }

    const auto hard_link = temporary.path() / "hard-link.log";
    std::filesystem::create_hard_link(victim, hard_link);
    try {
        onedrive::logging::Session session{{
            .file = hard_link,
        }};
        return fail("hard-linked log file was accepted");
    } catch (const std::exception&) {
    }
    if (onedrive::test::read_file(victim) != "unchanged") {
        return fail("hard-linked log target was modified");
    }

    const auto base = temporary.path() / "rotation.log";
    std::filesystem::create_symlink(victim, base.string() + ".1");
    try {
        onedrive::logging::Session session{{
            .file = base,
        }};
        return fail("symbolic-link rotation file was accepted");
    } catch (const std::exception&) {
    }
    return EXIT_SUCCESS;
}

int test_rejects_untrusted_directory() {
    TemporaryDirectory temporary;
    std::filesystem::permissions(
        temporary.path(), std::filesystem::perms::all
    );
    try {
        onedrive::logging::Session session{{
            .file = temporary.path() / "onedrive.log",
        }};
        return fail("log directory writable by other users was accepted");
    } catch (const std::exception&) {
    }
    return EXIT_SUCCESS;
}

int test_message_sink() {
    std::vector<
        std::pair<onedrive::logging::Severity, std::string>
    > messages;
    {
        onedrive::logging::Session session{{
            .level = "warn",
            .message_sink =
                [&messages](
                    onedrive::logging::Severity severity,
                    std::string_view message
                ) {
                    messages.emplace_back(severity, message);
                },
        }};
        spdlog::info("filtered");
        spdlog::warn("dashboard warning");
        spdlog::error("dashboard error");
    }
    if (messages.size() != 2 ||
        messages[0].first != onedrive::logging::Severity::warning ||
        messages[0].second != "dashboard warning" ||
        messages[1].first != onedrive::logging::Severity::error ||
        messages[1].second != "dashboard error") {
        return fail("logging message sink received incorrect events");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_level_parsing(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_private_log_and_rotation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_rejects_unsafe_log_files();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_rejects_untrusted_directory();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_message_sink();
}
