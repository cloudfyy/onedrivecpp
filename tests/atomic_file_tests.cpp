#include "detail/atomic_file.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace {

using onedrive::test::fail;

}  // namespace

int main() {
    const onedrive::test::TemporaryDirectory temporary;
    const auto destination = temporary.path() / "private";
    onedrive::detail::write_file_atomically(
        destination,
        "first",
        S_IRUSR | S_IWUSR,
        "test file"
    );
    struct stat status {};
    if (onedrive::test::read_file(destination) != "first" ||
        ::stat(destination.c_str(), &status) == -1 ||
        (status.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
        return fail("atomic test file was not created securely");
    }

    onedrive::detail::write_file_atomically(
        destination,
        "second",
        S_IRUSR | S_IWUSR,
        "test file"
    );
    if (onedrive::test::read_file(destination) != "second") {
        return fail("atomic test file was not replaced");
    }

    std::jthread first{[&] {
        onedrive::detail::write_file_atomically(
            destination,
            "concurrent-first",
            S_IRUSR | S_IWUSR,
            "test file"
        );
    }};
    std::jthread second{[&] {
        onedrive::detail::write_file_atomically(
            destination,
            "concurrent-second",
            S_IRUSR | S_IWUSR,
            "test file"
        );
    }};
    first.join();
    second.join();
    const auto contents = onedrive::test::read_file(destination);
    if (contents != "concurrent-first" &&
        contents != "concurrent-second") {
        return fail("concurrent atomic test file write was corrupted");
    }
    for (const auto& entry :
         std::filesystem::directory_iterator{temporary.path()}) {
        if (entry.path() != destination) {
            return fail("atomic test file write left a temporary file");
        }
    }

    const auto real_parent = temporary.path() / "real-parent";
    const auto linked_parent = temporary.path() / "linked-parent";
    std::filesystem::create_directory(real_parent);
    std::filesystem::create_directory_symlink(real_parent, linked_parent);
    try {
        onedrive::detail::write_file_atomically(
            linked_parent / "private",
            "data",
            S_IRUSR | S_IWUSR,
            "test file"
        );
        return fail("atomic test file followed a parent symlink");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(real_parent / "private")) {
        return fail("rejected atomic test file write modified its target");
    }
    return EXIT_SUCCESS;
}
