#include "onedrive/logging/logging.hpp"
#include "support/common.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

}  // namespace

int main() {
    if (const int result = test_private_log_and_rotation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_rejects_unsafe_log_files();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_rejects_untrusted_directory();
}
