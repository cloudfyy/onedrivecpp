#include "onedrive/logging/logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::logging {
namespace {

constexpr std::size_t maximum_log_file_size = 5U * 1024U * 1024U;
constexpr std::size_t retained_log_files = 3;

spdlog::level::level_enum parse_level(std::string level) {
    std::ranges::transform(level, level.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "info") {
        return spdlog::level::info;
    }
    if (level == "warn") {
        return spdlog::level::warn;
    }
    if (level == "error") {
        return spdlog::level::err;
    }
    if (level == "critical") {
        return spdlog::level::critical;
    }
    if (level == "off") {
        return spdlog::level::off;
    }
    throw std::invalid_argument("invalid log level: " + level);
}

}  // namespace

Session::Session(const Options& options) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    if (options.file) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.file->string(),
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
