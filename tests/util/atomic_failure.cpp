#include "support/common.hpp"
#include "util/atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

enum class Fault {
    none,
    permissions,
    write,
    zero_write,
    interrupted_partial_write,
    flush_file,
    close,
    rename,
    flush_directory,
    cleanup,
    collision,
    collisions_exhausted,
};

Fault fault = Fault::none;
int temporary_descriptor = -1;
int directory_descriptor = -1;
unsigned calls = 0;

bool temporary_name(const char* name) {
    return std::string_view{name}.starts_with(".private.tmp.");
}

int io_error() {
    errno = EIO;
    return -1;
}

} // namespace

extern "C" {
int __real_openat(int, const char*, int, ...);
int __real_fchmod(int, mode_t);
ssize_t __real_write(int, const void*, size_t);
int __real_fsync(int);
int __real_close(int);
int __real_renameat(int, const char*, int, const char*);
int __real_unlinkat(int, const char*, int);

int __wrap_openat(int directory, const char* name, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    if (fault != Fault::none && temporary_name(name)) {
        directory_descriptor = directory;
        if ((fault == Fault::collision ||
             fault == Fault::collisions_exhausted) &&
            (calls++ == 0 || fault == Fault::collisions_exhausted)) {
            errno = EEXIST;
            return -1;
        }
    }
    return __real_openat(directory, name, flags, mode);
}

int __wrap_fchmod(int descriptor, mode_t mode) {
    if (fault != Fault::none) {
        temporary_descriptor = descriptor;
        if (fault == Fault::permissions || fault == Fault::cleanup) {
            ++calls;
            return io_error();
        }
    }
    return __real_fchmod(descriptor, mode);
}

ssize_t __wrap_write(int descriptor, const void* data, size_t size) {
    if (descriptor == temporary_descriptor) {
        if (fault == Fault::write || fault == Fault::zero_write) {
            ++calls;
            return fault == Fault::write ? io_error() : 0;
        }
        if (fault == Fault::interrupted_partial_write) {
            if (calls++ == 0) {
                errno = EINTR;
                return -1;
            }
            return __real_write(descriptor, data, std::min(size, size_t{2}));
        }
    }
    return __real_write(descriptor, data, size);
}

int __wrap_fsync(int descriptor) {
    if ((descriptor == temporary_descriptor && fault == Fault::flush_file) ||
        (descriptor == directory_descriptor &&
         fault == Fault::flush_directory)) {
        ++calls;
        return io_error();
    }
    return __real_fsync(descriptor);
}

int __wrap_close(int descriptor) {
    const int result = __real_close(descriptor);
    if (descriptor == temporary_descriptor && fault == Fault::close) {
        ++calls;
        return io_error();
    }
    return result;
}

int __wrap_renameat(
    int source_directory,
    const char* source,
    int destination_directory,
    const char* destination
) {
    if (fault == Fault::rename && temporary_name(source)) {
        ++calls;
        return io_error();
    }
    return __real_renameat(
        source_directory, source, destination_directory, destination
    );
}

int __wrap_unlinkat(int directory, const char* name, int flags) {
    if (fault == Fault::cleanup && temporary_name(name)) {
        ++calls;
        errno = EACCES;
        return -1;
    }
    return __real_unlinkat(directory, name, flags);
}
}

int main() {
    using onedrive::test::fail;
    const onedrive::test::TemporaryDirectory temporary;
    const auto destination = temporary.path() / "private";
    struct Scenario {
        Fault fault;
        std::string_view message;
    };
    for (const auto& scenario : {
             Scenario{Fault::permissions, "cannot secure test file"},
             Scenario{Fault::write, "cannot write test file"},
             Scenario{Fault::zero_write, "cannot write test file"},
             Scenario{Fault::flush_file, "cannot flush test file"},
             Scenario{Fault::close, "cannot close test file"},
             Scenario{Fault::rename, "cannot replace test file"},
             Scenario{Fault::flush_directory, "cannot flush parent directory"},
             Scenario{
                 Fault::cleanup, "additionally cannot remove temporary file"
             },
             Scenario{
                 Fault::collisions_exhausted,
                 "too many temporary file collisions"
             },
         }) {
        onedrive::test::write_file(destination, "original");
        calls = 0;
        temporary_descriptor = -1;
        directory_descriptor = -1;
        fault = scenario.fault;
        const bool rejected = onedrive::test::throws_with(
            [&] {
                onedrive::util::write_file_atomically(
                    destination, "replacement", 0600, "test file"
                );
            },
            scenario.message
        );
        fault = Fault::none;
        if (!rejected || calls == 0 ||
            (scenario.fault == Fault::collisions_exhausted && calls != 128)) {
            return fail(
                "atomic syscall failure did not report the expected error"
            );
        }
        const auto expected = scenario.fault == Fault::flush_directory
                                  ? "replacement"
                                  : "original";
        if (onedrive::test::read_file(destination) != expected) {
            return fail("atomic failure violated the rename commit boundary");
        }
        unsigned leftovers = 0;
        for (const auto& entry :
             std::filesystem::directory_iterator{temporary.path()}) {
            if (entry.path() != destination) {
                ++leftovers;
                std::filesystem::remove(entry.path());
            }
        }
        if (leftovers != (scenario.fault == Fault::cleanup ? 1U : 0U)) {
            return fail("atomic failure did not clean up its temporary file");
        }
    }
    for (const auto retry :
         {Fault::collision, Fault::interrupted_partial_write}) {
        calls = 0;
        temporary_descriptor = -1;
        fault = retry;
        onedrive::util::write_file_atomically(
            destination, "complete-payload", 0600, "test file"
        );
        fault = Fault::none;
        if (calls < 2 ||
            onedrive::test::read_file(destination) != "complete-payload" ||
            std::distance(
                std::filesystem::directory_iterator{temporary.path()},
                std::filesystem::directory_iterator{}
            ) != 1) {
            return fail("atomic retry lost data or leaked a temporary file");
        }
    }
    return EXIT_SUCCESS;
}
