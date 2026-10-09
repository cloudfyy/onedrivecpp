#include "util/private_file.hpp"
#include "support/common.hpp"

#include <sys/stat.h>
#include <unistd.h>

namespace {

using onedrive::test::fail;
using onedrive::test::throws_with;
using onedrive::util::PrivateFileRequirements;
using onedrive::util::read_private_file;

int test_size_boundaries() {
    onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    for (const std::size_t limit : {0U, 1U, 4096U, 65536U, 1048576U}) {
        const PrivateFileRequirements requirements{.maximum_size = limit};
        const std::string contents(limit, 'x');
        onedrive::test::write_file(path, contents);
        if (read_private_file(path, "test", requirements) != contents) {
            return fail("private file rejected or changed size-limit contents");
        }
        onedrive::test::write_file(path, contents + "x");
        if (!throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        read_private_file(path, "test", requirements)
                    );
                },
                std::to_string(limit) + " byte size limit"
            )) {
            return fail("private file did not reject limit plus one byte");
        }
    }
    const std::string binary{"a\0b\n", 4};
    onedrive::test::write_file(path, binary);
    if (read_private_file(path, "test", {}) != binary) {
        return fail("private file reader did not preserve binary contents");
    }
    return EXIT_SUCCESS;
}

int test_permissions_and_links() {
    onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    onedrive::test::write_file(path, "secret");
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    PrivateFileRequirements requirements{
        .required_owner = ::geteuid(),
        .exact_permissions = 0600,
        .permission_requirement = "requires private permissions",
        .require_single_link = true,
    };
    if (read_private_file(path, "test", requirements) != "secret") {
        return fail("private owned file was rejected");
    }
    requirements.required_owner = ::geteuid() == 0 ? 1 : 0;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file accepted an unexpected owner");
    }
    requirements.required_owner = ::geteuid();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read);
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file ignored exact permissions");
    }
    requirements.exact_permissions.reset();
    requirements.forbidden_permissions = S_IRGRP;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read
    );
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file ignored forbidden permissions");
    }
    requirements.forbidden_permissions = 0;
    std::filesystem::create_hard_link(path, temporary.path() / "hard-link");
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "exactly one hard link"
        )) {
        return fail("private file accepted multiple links when forbidden");
    }
    requirements.require_single_link = false;
    if (read_private_file(path, "test", requirements) != "secret") {
        return fail("private file rejected allowed hard links");
    }
    return EXIT_SUCCESS;
}

int test_non_regular_paths() {
    onedrive::test::TemporaryDirectory temporary;
    const auto fifo = temporary.path() / "fifo";
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        return fail("cannot create private file FIFO fixture");
    }
    for (const auto& path : {temporary.path(), fifo}) {
        if (!throws_with<std::runtime_error>(
                [&] { static_cast<void>(read_private_file(path, "test", {})); },
                "not a regular file"
            )) {
            return fail("private file reader accepted a directory or FIFO");
        }
    }
    const auto file = temporary.path() / "file";
    const auto link = temporary.path() / "symlink";
    onedrive::test::write_file(file, "secret");
    std::filesystem::create_symlink(file, link);
    if (!throws_with<std::runtime_error>([&] {
            static_cast<void>(read_private_file(link, "test", {}));
        }) ||
        !throws_with<std::runtime_error>([&] {
            static_cast<void>(
                read_private_file(temporary.path() / "missing", "test", {})
            );
        })) {
        return fail("private file accepted a symlink or missing path");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    for (auto test :
         {test_size_boundaries,
          test_permissions_and_links,
          test_non_regular_paths}) {
        if (const int result = test(); result != EXIT_SUCCESS) {
            return result;
        }
    }
    return EXIT_SUCCESS;
}
