#include "support/common.hpp"
#include "sync/filesystem/metadata.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace {

enum class Operation { none, set, get, remove, close };
Operation operation = Operation::none;
int injected_error = 0;
bool corrupt_value = false;
bool short_value = false;
bool missing_probe = false;
bool block_cleanup = false;
std::filesystem::path probe;
std::filesystem::path identity_file;
unsigned injected = 0;
unsigned fail_identity_call = 0;
int identity_descriptor = -1;
std::vector<std::pair<std::string, std::string>> identity_attributes;

bool matches(int descriptor, const std::filesystem::path& path) {
    struct stat actual{};
    struct stat expected{};
    return !path.empty() && ::fstat(descriptor, &actual) == 0 &&
           ::stat(path.c_str(), &expected) == 0 &&
           actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
}

void obstruct_cleanup() {
    std::filesystem::remove(probe);
    std::filesystem::create_directory(probe);
    onedrive::test::write_file(probe / "child", "keep");
}

int inject() {
    ++injected;
    errno = injected_error;
    return -1;
}

} // namespace

extern "C" {
int __real_setxattr(const char*, const char*, const void*, size_t, int);
ssize_t __real_getxattr(const char*, const char*, void*, size_t);
int __real_removexattr(const char*, const char*);
int __real_fsetxattr(int, const char*, const void*, size_t, int);
int __real_close(int);

int __wrap_setxattr(
    const char* path,
    const char* name,
    const void* value,
    size_t size,
    int flags
) {
    if (path != probe) {
        return __real_setxattr(path, name, value, size, flags);
    }
    if (operation == Operation::set) {
        if (block_cleanup) {
            obstruct_cleanup();
        }
        return inject();
    }
    return 0;
}

ssize_t
__wrap_getxattr(const char* path, const char* name, void* value, size_t size) {
    if (path != probe) {
        return __real_getxattr(path, name, value, size);
    }
    if (operation == Operation::get) {
        return inject();
    }
    const std::string_view result = corrupt_value ? "different"
                                    : short_value ? "short"
                                                  : "supported";
    if (size < result.size()) {
        errno = ERANGE;
        return -1;
    }
    std::memcpy(value, result.data(), result.size());
    return static_cast<ssize_t>(result.size());
}

int __wrap_removexattr(const char* path, const char* name) {
    if (path != probe) {
        return __real_removexattr(path, name);
    }
    if (missing_probe) {
        std::filesystem::remove(probe);
    } else if (block_cleanup) {
        obstruct_cleanup();
    }
    return operation == Operation::remove ? inject() : 0;
}

int __wrap_fsetxattr(
    int descriptor, const char* name, const void* value, size_t size, int flags
) {
    if (!matches(descriptor, identity_file)) {
        return __real_fsetxattr(descriptor, name, value, size, flags);
    }
    identity_descriptor = descriptor;
    identity_attributes.emplace_back(
        name, std::string{static_cast<const char*>(value), size}
    );
    if (identity_attributes.size() == fail_identity_call) {
        return inject();
    }
    return 0;
}

int __wrap_close(int descriptor) {
    const bool is_probe = matches(descriptor, probe);
    const bool selected = operation == Operation::close &&
                          (is_probe || matches(descriptor, identity_file));
    const int result = __real_close(descriptor);
    if (selected) {
        if (is_probe && block_cleanup) {
            obstruct_cleanup();
        }
        return inject();
    }
    return result;
}
}

int main() {
    using onedrive::config::FilesystemMetadataMode;
    using onedrive::sync::detail::FilesystemMetadata;
    using onedrive::test::fail;
    const onedrive::test::TemporaryDirectory temporary;
    probe = temporary.path() /
            (".onedrive-cpp-xattr-probe-" + std::to_string(::getpid()));
    for (const auto selected :
         {Operation::set, Operation::get, Operation::remove}) {
        for (const int error : {ENOTSUP, EPERM, EACCES, ENODATA, EIO}) {
            operation = selected;
            injected_error = error;
            injected = 0;
            if (error == EIO) {
                const std::string_view message =
                    selected == Operation::set
                        ? "cannot probe user extended attributes"
                    : selected == Operation::get
                        ? "cannot read filesystem user extended attribute probe"
                        : "cannot remove filesystem user extended attribute "
                          "probe";
                if (!onedrive::test::throws_with(
                        [&] {
                            static_cast<void>(FilesystemMetadata::detect(
                                FilesystemMetadataMode::automatic,
                                temporary.path()
                            ));
                        },
                        message
                    )) {
                    return fail(
                        "unexpected xattr probe failure was silently downgraded"
                    );
                }
            } else {
                if (FilesystemMetadata::detect(
                        FilesystemMetadataMode::automatic, temporary.path()
                    )
                        .uses_xattrs()) {
                    return fail(
                        "unavailable xattr probe did not fall back to database"
                    );
                }
                if (!onedrive::test::throws_with(
                        [&] {
                            static_cast<void>(FilesystemMetadata::detect(
                                FilesystemMetadataMode::xattr, temporary.path()
                            ));
                        },
                        "requires user extended attribute support"
                    )) {
                    return fail(
                        "required xattr mode silently fell back to database"
                    );
                }
            }
            if (injected == 0 || std::filesystem::exists(probe)) {
                return fail("failed xattr probe did not clean up its file");
            }
        }
    }
    operation = Operation::none;
    if (!FilesystemMetadata::detect(
             FilesystemMetadataMode::automatic, temporary.path()
        )
             .uses_xattrs()) {
        return fail("successful xattr round trip did not enable metadata");
    }
    for (const bool short_result : {false, true}) {
        corrupt_value = !short_result;
        short_value = short_result;
        if (FilesystemMetadata::detect(
                FilesystemMetadataMode::automatic, temporary.path()
            )
                .uses_xattrs() ||
            std::filesystem::exists(probe)) {
            return fail(
                "corrupt xattr round trip enabled metadata or leaked probe"
            );
        }
    }
    corrupt_value = false;
    short_value = false;
    operation = Operation::close;
    injected_error = EIO;
    injected = 0;
    if (!onedrive::test::throws_with(
            [&] {
                static_cast<void>(FilesystemMetadata::detect(
                    FilesystemMetadataMode::automatic, temporary.path()
                ));
            },
            "cannot close filesystem capability probe"
        ) ||
        injected != 1 || std::filesystem::exists(probe)) {
        return fail("probe close failure was not reported and cleaned up");
    }
    operation = Operation::none;
    for (const bool absent : {true, false}) {
        missing_probe = absent;
        block_cleanup = !absent;
        if (!onedrive::test::throws_with(
                [&] {
                    static_cast<void>(FilesystemMetadata::detect(
                        FilesystemMetadataMode::automatic, temporary.path()
                    ));
                },
                "cannot remove filesystem capability probe"
            )) {
            return fail("probe cleanup failure was silently accepted");
        }
        std::filesystem::remove_all(probe);
    }
    missing_probe = false;
    block_cleanup = false;
    for (const auto selected : {Operation::set, Operation::close}) {
        operation = selected;
        block_cleanup = true;
        const std::string_view expected =
            selected == Operation::set
                ? "cannot probe user extended attributes"
                : "cannot close filesystem capability probe";
        if (!onedrive::test::throws_with(
                [&] {
                    static_cast<void>(FilesystemMetadata::detect(
                        FilesystemMetadataMode::automatic, temporary.path()
                    ));
                },
                expected
            ) ||
            !std::filesystem::exists(probe / "child")) {
            return fail(
                "probe cleanup warning masked the original syscall failure"
            );
        }
        std::filesystem::remove_all(probe);
    }
    operation = Operation::none;
    block_cleanup = false;

    identity_file = temporary.path() / "identity";
    onedrive::test::write_file(identity_file, "data");
    const auto metadata = FilesystemMetadata::from_detected_support(
        FilesystemMetadataMode::xattr, true
    );
    const onedrive::graph::RemoteItem item{.id = "remote", .etag = "etag"};
    for (const unsigned failed_call : {1U, 2U, 3U, 0U}) {
        fail_identity_call = failed_call;
        identity_attributes.clear();
        injected = 0;
        if (failed_call != 0) {
            if (!onedrive::test::throws_with(
                    [&] {
                        metadata.write_remote_identity(item, identity_file);
                    },
                    "cannot write synchronization metadata"
                ) ||
                identity_attributes.size() != failed_call || injected != 1) {
                return fail(
                    "identity xattr failure was not propagated at its write"
                );
            }
        } else {
            operation = Operation::close;
            if (!onedrive::test::throws_with(
                    [&] {
                        metadata.write_remote_identity(item, identity_file);
                    },
                    "cannot close synchronization metadata file"
                ) ||
                identity_attributes.size() != 3 || injected != 1) {
                return fail("identity close failure was not propagated");
            }
            operation = Operation::none;
        }
        errno = 0;
        if (::fcntl(identity_descriptor, F_GETFD) != -1 || errno != EBADF ||
            onedrive::test::read_file(identity_file) != "data") {
            return fail(
                "identity failure leaked a descriptor or changed file contents"
            );
        }
    }
    return EXIT_SUCCESS;
}
