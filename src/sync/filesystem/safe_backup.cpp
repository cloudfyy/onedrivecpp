#include "sync/filesystem/safe_backup.hpp"

#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/sha256.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

using Descriptor = onedrive::util::UniqueFD;

constexpr std::size_t maximum_backup_filename_bytes = 240;
constexpr std::size_t backup_filename_hash_characters = 16;
constexpr unsigned maximum_backup_sequence = 9999;
constexpr std::size_t copy_buffer_bytes = std::size_t{64} * 1024U;

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    if (::gmtime_r(&seconds, &value) == nullptr) {
        throw std::runtime_error("cannot create safeBackup timestamp");
    }
    std::array<char, 17> buffer{};
    if (::strftime(
            buffer.data(),
            buffer.size(),
            "%Y%m%dT%H%M%SZ",
            &value
        ) == 0) {
        throw std::runtime_error("cannot format safeBackup timestamp");
    }
    return buffer.data();
}

std::string backup_filename(
    const std::filesystem::path& source,
    const std::string& time,
    unsigned sequence
) {
    auto stem = source.stem().string();
    auto extension = source.extension().string();
    const auto suffix = std::format(
        ".safeBackup-{}-{:04}",
        time,
        sequence
    );
    if (stem.size() + suffix.size() + extension.size() >
        maximum_backup_filename_bytes) {
        stem = "onedrive-" +
               util::sha256_hex(source.filename().string()).substr(
                   0, backup_filename_hash_characters
               );
        if (stem.size() + suffix.size() + extension.size() >
            maximum_backup_filename_bytes) {
            extension.clear();
        }
    }
    return stem + suffix + extension;
}

void write_all(int descriptor, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::write(
            descriptor,
            data + written,
            size - written
        );
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                "cannot write safeBackup: " +
                std::string{onedrive::util::system_error_message(errno)}
            );
        }
        written += static_cast<std::size_t>(result);
    }
}

void copy_source(
    const SafeSyncRoot& sync_root,
    const std::filesystem::path& source,
    const std::filesystem::path& temporary,
    const LocalFileBaseline& baseline
) {
    Descriptor input{sync_root.open(source, O_RDONLY)};
    struct stat source_status {};
    if (::fstat(input.get(), &source_status) == -1 ||
        !S_ISREG(source_status.st_mode) ||
        source_status.st_size != baseline.size ||
        modified_ticks(input.get()) != baseline.modified_ticks) {
        throw LocalModificationConflictError(
            "local file changed before safeBackup creation: " +
            source.string()
        );
    }

    Descriptor output{sync_root.open(
        temporary,
        O_WRONLY | O_CREAT | O_EXCL,
        S_IRUSR | S_IWUSR
    )};
    try {
        std::array<char, copy_buffer_bytes> buffer{};
        while (true) {
            const auto count = ::read(
                input.get(),
                buffer.data(),
                buffer.size()
            );
            if (count == 0) {
                break;
            }
            if (count == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(
                    "cannot read local file for safeBackup: " +
                    std::string{onedrive::util::system_error_message(errno)}
                );
            }
            write_all(
                output.get(),
                buffer.data(),
                static_cast<std::size_t>(count)
            );
        }
        if (::fchmod(
                output.get(),
                source_status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)
            ) == -1 ||
            ::fsync(output.get()) == -1) {
            throw std::runtime_error(
                "cannot flush safeBackup: " +
                std::string{onedrive::util::system_error_message(errno)}
            );
        }
    } catch (...) {
        remove_no_symlinks(temporary);
        throw;
    }
}

}  // namespace

SafeBackup preserve_safe_backup(
    const SafeSyncRoot& sync_root,
    const std::filesystem::path& source,
    const LocalFileBaseline& baseline
) {
    if (!baseline.existed || baseline.fingerprint.empty()) {
        throw std::invalid_argument(
            "safeBackup requires an existing local-file baseline"
        );
    }
    const auto time = timestamp();
    for (unsigned sequence = 1;
         sequence <= maximum_backup_sequence; ++sequence) {
        const auto backup = source.parent_path() /
                            backup_filename(source, time, sequence);
        const auto temporary = temporary_path_for(backup);
        copy_source(sync_root, source, temporary, baseline);
        if (content_fingerprint(temporary) != baseline.fingerprint ||
            !local_file_matches_baseline(source, baseline)) {
            remove_no_symlinks(temporary);
            throw LocalModificationConflictError(
                "local file changed while creating safeBackup: " +
                source.string()
            );
        }
        if (!sync_root.rename_no_replace(temporary, backup)) {
            remove_no_symlinks(temporary);
            continue;
        }
        sync_root.fsync_directory(backup.parent_path());
        return {
            .path = backup,
            .fingerprint = baseline.fingerprint,
        };
    }
    throw std::runtime_error(
        "cannot allocate a unique safeBackup name for: " + source.string()
    );
}

SafeBackup preserve_displaced_backup(
    const SafeSyncRoot& sync_root,
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) {
    const auto time = timestamp();
    for (unsigned sequence = 1; sequence <= maximum_backup_sequence;
         ++sequence) {
        const auto backup = destination.parent_path() /
                            backup_filename(destination, time, sequence);
        if (!sync_root.rename_no_replace(source, backup)) {
            continue;
        }
        sync_root.fsync_directory(backup.parent_path());
        const auto baseline = capture_local_file_baseline(backup);
        if (!baseline.existed) {
            throw LocalModificationConflictError(
                "displaced safeBackup disappeared: " + backup.string()
            );
        }
        fsync_file(backup);
        return {.path = backup, .fingerprint = baseline.fingerprint};
    }
    throw std::runtime_error(
        "cannot allocate a unique displaced safeBackup name for: " +
        destination.string()
    );
}

}  // namespace onedrive::sync::detail
