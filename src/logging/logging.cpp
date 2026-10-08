#include "onedrive/logging/logging.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "util/ascii.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace onedrive::logging {
namespace {

constexpr std::size_t maximum_log_file_size =
    std::size_t{5} * 1024U * 1024U;
constexpr std::size_t retained_log_files = 3;
constexpr mode_t private_log_mode = S_IRUSR | S_IWUSR;

Severity severity_for(spdlog::level::level_enum level) noexcept {
    switch (level) {
        case spdlog::level::trace:
            return Severity::trace;
        case spdlog::level::debug:
            return Severity::debug;
        case spdlog::level::info:
            return Severity::information;
        case spdlog::level::warn:
            return Severity::warning;
        case spdlog::level::err:
            return Severity::error;
        case spdlog::level::critical:
            return Severity::critical;
        case spdlog::level::off:
        case spdlog::level::n_levels:
            return Severity::information;
    }
    return Severity::information;
}

class MessageCallbackSink final
    : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit MessageCallbackSink(MessageSink sink)
        : sink_{std::move(sink)} {}

private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        sink_(
            severity_for(message.level),
            std::string_view{message.payload.data(), message.payload.size()}
        );
    }

    void flush_() override {}

    MessageSink sink_;
};

[[noreturn]] void throw_log_error(
    std::string_view operation,
    const std::filesystem::path& path,
    int error
) {
    util::throw_system_error(
        error,
        std::string{operation} + " '" + path.string() + "'"
    );
}

class SecureRotatingFileSink final
    : public spdlog::sinks::base_sink<std::mutex> {
public:
    SecureRotatingFileSink(
        const std::filesystem::path& path,
        std::size_t maximum_size,
        std::size_t retained_files
    )
        : path_{util::normalized_absolute(path)},
          filename_{path_.filename().native()},
          maximum_size_{maximum_size},
          retained_files_{retained_files} {
        if (filename_.empty() || filename_ == "." || filename_ == "..") {
            throw std::invalid_argument(
                "log file path must identify a file: " + path.string()
            );
        }
        const auto parent = path_.parent_path();
        directory_ = util::open_path_no_symlinks(
            parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW
        );
        validate_directory(parent);
        for (std::size_t index = 1; index <= retained_files_; ++index) {
            static_cast<void>(
                validate_existing_file(rotated_filename(index))
            );
        }
        open_file();
    }

private:
    void validate_directory(const std::filesystem::path& parent) const {
        const auto status = util::inspect_owned_directory(
            directory_.get(), parent, "log directory"
        );
        if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            throw std::runtime_error(
                "log directory must not be writable by other users: " +
                parent.string()
            );
        }
    }

    [[nodiscard]] bool validate_existing_file(
        const std::string& filename
    ) const {
        const auto file_path = path_.parent_path() / filename;
        util::UniqueFD descriptor{
            ::openat(
                directory_.get(),
                filename.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
            )
        };
        if (!descriptor) {
            if (errno == ENOENT) {
                return false;
            }
            throw_log_error("cannot open log file", file_path, errno);
        }
        static_cast<void>(util::secure_owned_regular_file(
            descriptor.get(),
            file_path,
            private_log_mode,
            "log file"
        ));
        return true;
    }

    void open_file() {
        util::UniqueFD descriptor{
            ::openat(
                directory_.get(),
                filename_.c_str(),
                O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                private_log_mode
            )
        };
        if (!descriptor) {
            throw_log_error("cannot open log file", path_, errno);
        }
        const auto status = util::secure_owned_regular_file(
            descriptor.get(), path_, private_log_mode, "log file"
        );
        size_ = static_cast<std::uintmax_t>(status.st_size);
        file_ = std::move(descriptor);
    }

    [[nodiscard]] std::string rotated_filename(std::size_t index) const {
        return std::format("{}.{}", filename_, index);
    }

    void remove_existing(const std::string& filename) {
        if (!validate_existing_file(filename)) {
            return;
        }
        if (::unlinkat(directory_.get(), filename.c_str(), 0) == -1) {
            throw_log_error(
                "cannot remove rotated log file",
                path_.parent_path() / filename,
                errno
            );
        }
    }

    void rotate() {
        if (const auto error = file_.close(); error) {
            throw std::system_error{
                error, "cannot close log file before rotation"
            };
        }
        if (retained_files_ != 0) {
            remove_existing(rotated_filename(retained_files_));
            for (std::size_t index = retained_files_; index > 1; --index) {
                const auto source = rotated_filename(index - 1);
                const auto destination = rotated_filename(index);
                if (!validate_existing_file(source)) {
                    continue;
                }
                remove_existing(destination);
                if (::renameat(
                        directory_.get(),
                        source.c_str(),
                        directory_.get(),
                        destination.c_str()
                    ) == -1) {
                    throw_log_error(
                        "cannot rotate log file",
                        path_.parent_path() / source,
                        errno
                    );
                }
            }
            if (validate_existing_file(filename_)) {
                remove_existing(rotated_filename(1));
                if (::renameat(
                        directory_.get(),
                        filename_.c_str(),
                        directory_.get(),
                        rotated_filename(1).c_str()
                    ) == -1) {
                    throw_log_error("cannot rotate log file", path_, errno);
                }
            }
        } else {
            remove_existing(filename_);
        }
        open_file();
    }

    void sink_it_(const spdlog::details::log_msg& message) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        if (size_ != 0 &&
            size_ + formatted.size() > maximum_size_) {
            rotate();
        }
        std::size_t offset = 0;
        while (offset < formatted.size()) {
            const auto result = ::write(
                file_.get(),
                formatted.data() + offset,
                formatted.size() - offset
            );
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result == -1) {
                throw_log_error("cannot write log file", path_, errno);
            }
            offset += static_cast<std::size_t>(result);
        }
        size_ += formatted.size();
    }

    void flush_() override {
        if (::fsync(file_.get()) == -1) {
            throw_log_error("cannot flush log file", path_, errno);
        }
    }

    std::filesystem::path path_;
    std::string filename_;
    util::UniqueFD directory_;
    util::UniqueFD file_;
    std::size_t maximum_size_;
    std::size_t retained_files_;
    std::uintmax_t size_{0};
};

spdlog::level::level_enum parse_level(std::string_view level) {
    if (util::ascii_iequals(level, "trace")) {
        return spdlog::level::trace;
    }
    if (util::ascii_iequals(level, "debug")) {
        return spdlog::level::debug;
    }
    if (util::ascii_iequals(level, "info")) {
        return spdlog::level::info;
    }
    if (util::ascii_iequals(level, "warn")) {
        return spdlog::level::warn;
    }
    if (util::ascii_iequals(level, "error")) {
        return spdlog::level::err;
    }
    if (util::ascii_iequals(level, "critical")) {
        return spdlog::level::critical;
    }
    if (util::ascii_iequals(level, "off")) {
        return spdlog::level::off;
    }
    throw std::invalid_argument(std::format("invalid log level: {}", level));
}

}  // namespace

Session::Session(const Options& options) {
    std::vector<spdlog::sink_ptr> sinks;
    if (options.message_sink) {
        sinks.push_back(std::make_shared<MessageCallbackSink>(
            options.message_sink
        ));
    } else {
        sinks.push_back(
            std::make_shared<spdlog::sinks::stderr_color_sink_mt>()
        );
    }
    if (options.file) {
        sinks.push_back(std::make_shared<SecureRotatingFileSink>(
            *options.file,
            maximum_log_file_size,
            retained_log_files
        ));
    }

    auto logger = std::make_shared<spdlog::logger>(
        "onedrive-cpp",
        sinks.begin(),
        sinks.end()
    );
    logger->set_level(parse_level(options.level));
    logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
}

Session::~Session() {
    spdlog::shutdown();
}

}  // namespace onedrive::logging
