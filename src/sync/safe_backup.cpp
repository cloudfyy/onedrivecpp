#include "safe_backup.hpp"

#include "onedrive/sha256.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

class Descriptor {
public:
    explicit Descriptor(int descriptor) noexcept
        : descriptor_{descriptor} {}
    ~Descriptor() {
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

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
    if (stem.size() + suffix.size() + extension.size() > 240) {
        stem = "onedrive-" +
               sha256_hex(source.filename().string()).substr(0, 16);
        if (stem.size() + suffix.size() + extension.size() > 240) {
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
                std::string{std::strerror(errno)}
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
        std::array<char, std::size_t{64} * 1024U> buffer{};
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
                    std::string{std::strerror(errno)}
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
                std::string{std::strerror(errno)}
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
    for (unsigned sequence = 1; sequence <= 9999; ++sequence) {
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

}  // namespace onedrive::sync::detail
