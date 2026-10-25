#include "onedrive/logging/logging.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "util/ascii.hpp"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
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
constexpr const char* event_logger_name = "onedrive-cpp-events";

spdlog::level::level_enum level_for(Severity severity) {
    switch (severity) {
    case Severity::trace:
        return spdlog::level::trace;
    case Severity::debug:
        return spdlog::level::debug;
    case Severity::information:
        return spdlog::level::info;
    case Severity::warning:
        return spdlog::level::warn;
    case Severity::error:
        return spdlog::level::err;
    case Severity::critical:
        return spdlog::level::critical;
    }
    throw std::logic_error{"unknown log severity"};
}

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

class MessageCallbackSink final : public spdlog::sinks::sink {
public:
    explicit MessageCallbackSink(MessageSink message_sink)
        : sink_{std::move(message_sink)} {
    }

    void disconnect() {
        const std::scoped_lock lock{mutex_};
        sink_ = {};
    }

    void log(const spdlog::details::log_msg& message) override {
        const std::scoped_lock lock{mutex_};
        if (sink_) {
            sink_(
                severity_for(message.level),
                std::string_view{message.payload.data(), message.payload.size()}
            );
        }
    }

    void flush() override {}

    void set_pattern(const std::string&) override {}

    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

private:
    std::mutex mutex_;
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

class SecureRotatingFileSink final : public spdlog::sinks::sink {
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

    void log(const spdlog::details::log_msg& message) override {
        const std::scoped_lock lock{mutex_};
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

    void flush() override {
        const std::scoped_lock lock{mutex_};
        if (::fsync(file_.get()) == -1) {
            throw_log_error("cannot flush log file", path_, errno);
        }
    }

    void set_pattern(const std::string& pattern) override {
        const std::scoped_lock lock{mutex_};
        formatter_ = std::make_unique<spdlog::pattern_formatter>(pattern);
    }

    void set_formatter(
        std::unique_ptr<spdlog::formatter> sink_formatter
    ) override {
        const std::scoped_lock lock{mutex_};
        formatter_ = std::move(sink_formatter);
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

    std::mutex mutex_;
    std::unique_ptr<spdlog::formatter> formatter_{
        std::make_unique<spdlog::pattern_formatter>()
    };
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

struct SessionLoggers {
    std::shared_ptr<spdlog::logger> diagnostic;
    std::shared_ptr<spdlog::logger> event;
    std::shared_ptr<MessageCallbackSink> callback;
};

class RoutingSink final : public spdlog::sinks::sink {
public:
    explicit RoutingSink(const std::shared_ptr<spdlog::logger>& logger)
        : default_{logger},
          fallback_{logger->clone(logger->name())} {
    }

    void attach(const std::shared_ptr<SessionLoggers>& session) {
        const std::scoped_lock lock{mutex_};
        if (sessions_.empty()) {
            update_fallback_level();
        }
        sessions_.push_back(session);
        update_level();
    }

    void detach(const std::shared_ptr<SessionLoggers>& session) {
        const std::scoped_lock lock{mutex_};
        std::erase(sessions_, session);
        update_level();
    }

    [[nodiscard]] std::shared_ptr<spdlog::logger> current(bool event) {
        const std::scoped_lock lock{mutex_};
        if (sessions_.empty()) {
            update_fallback_level();
            return fallback_;
        }
        return event ? sessions_.back()->event : sessions_.back()->diagnostic;
    }

    void log(const spdlog::details::log_msg& message) override {
        current(false)->log(
            message.time, message.source, message.level, message.payload
        );
    }

    void flush() override {
        current(false)->flush();
    }

    void set_pattern(const std::string& pattern) override {
        current(false)->set_pattern(pattern);
    }

    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
        current(false)->set_formatter(std::move(formatter));
    }

private:
    void update_fallback_level() {
        if (const auto logger = default_.lock()) {
            fallback_->set_level(logger->level());
        }
    }

    void update_level() {
        if (const auto logger = default_.lock()) {
            logger->set_level(
                sessions_.empty() ? fallback_->level()
                                  : sessions_.back()->diagnostic->level()
            );
        }
    }

    std::mutex mutex_;
    std::weak_ptr<spdlog::logger> default_;
    std::shared_ptr<spdlog::logger> fallback_;
    std::vector<std::shared_ptr<SessionLoggers>> sessions_;
};

const std::shared_ptr<RoutingSink>& routing_sink() {
    static const auto sink = [] {
        const auto logger = spdlog::default_logger();
        if (!logger) {
            throw std::logic_error("logging requires an active default logger");
        }
        auto routing = std::make_shared<RoutingSink>(logger);
        logger->sinks() = {routing};
        return routing;
    }();
    return sink;
}

}  // namespace

struct Session::Implementation {
    std::shared_ptr<SessionLoggers> loggers;
    std::shared_ptr<RoutingSink> routing;
};

void initialize() {
    static_cast<void>(routing_sink());
}

void write_event(Severity severity, std::string_view message) {
    routing_sink()->current(true)->log(level_for(severity), "{}", message);
}

Session::Session(const Options& options) {
    const auto level = parse_level(options.level);
    auto session = std::make_shared<SessionLoggers>();
    std::vector<spdlog::sink_ptr> sinks;
    if (options.message_sink) {
        session->callback =
            std::make_shared<MessageCallbackSink>(options.message_sink);
        sinks.push_back(session->callback);
    } else {
        sinks.push_back(
            std::make_shared<spdlog::sinks::stderr_color_sink_mt>()
        );
    }
    auto event_sinks =
        options.message_sink ? std::vector<spdlog::sink_ptr>{} : sinks;
    if (options.file) {
        const auto file_sink = std::make_shared<SecureRotatingFileSink>(
            *options.file, maximum_log_file_size, retained_log_files
        );
        sinks.push_back(file_sink);
        event_sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>(
        "onedrive-cpp",
        sinks.begin(),
        sinks.end()
    );
    logger->set_level(level);
    logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
    logger->flush_on(spdlog::level::warn);
    auto event_logger = logger->clone(event_logger_name);
    event_logger->sinks() = std::move(event_sinks);
    session->diagnostic = std::move(logger);
    session->event = std::move(event_logger);
    auto implementation =
        std::make_unique<Implementation>(std::move(session), routing_sink());
    implementation->routing->attach(implementation->loggers);
    implementation_ = std::move(implementation);
}

Session::~Session() {
    implementation_->routing->detach(implementation_->loggers);
    if (implementation_->loggers->callback) {
        implementation_->loggers->callback->disconnect();
    }
}

}  // namespace onedrive::logging
