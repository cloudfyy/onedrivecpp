#include "onedrive/config/config.hpp"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace onedrive::config {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_bool(std::string_view value, std::size_t line_number) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error(
        "invalid boolean at config line " + std::to_string(line_number)
    );
}

std::uint64_t parse_unsigned(
    std::string_view value,
    std::string_view key,
    std::size_t line_number
) {
    std::uint64_t result{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, result);
    if (value.empty() || error != std::errc{} || position != end) {
        throw std::runtime_error(
            "invalid unsigned integer for '" + std::string{key} +
            "' at config line " + std::to_string(line_number)
        );
    }
    return result;
}

std::chrono::seconds parse_seconds(
    std::string_view value,
    std::string_view key,
    std::size_t line_number
) {
    const auto result = parse_unsigned(value, key, line_number);
    using SecondsRepresentation = std::chrono::seconds::rep;
    if (result >
        static_cast<std::uint64_t>(
            std::numeric_limits<SecondsRepresentation>::max()
        )) {
        throw std::runtime_error(
            "value for '" + std::string{key} + "' is too large at config line " +
            std::to_string(line_number)
        );
    }
    return std::chrono::seconds{static_cast<SecondsRepresentation>(result)};
}

}  // namespace

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .sync_directory = std::filesystem::path{home} / "OneDrive",
        .state_directory = std::filesystem::path{home} / ".local/state/onedrive-cpp",
        .drive_id = "me",
        .application_id = {},
        .azure_tenant_id = "common",
        .auth_endpoint = "https://login.microsoftonline.com",
        .auth_scope =
            "Files.ReadWrite Files.ReadWrite.All Sites.ReadWrite.All offline_access",
        .graph_maximum_throttle_retries = 4,
        .graph_initial_throttle_delay = std::chrono::seconds{1},
        .graph_maximum_throttle_delay = std::chrono::seconds{300},
        .dry_run = false,
    };
}

Config Config::load(const std::filesystem::path& path) {
    Config config = defaults();
    if (!std::filesystem::exists(path)) {
        spdlog::debug("Configuration file not found; using default values");
        return config;
    }

    spdlog::debug("Loading configuration file");
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path.string());
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "missing '=' at config line " + std::to_string(line_number)
            );
        }

        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key == "sync_directory") {
            config.sync_directory = value;
        } else if (key == "state_directory") {
            config.state_directory = value;
        } else if (key == "drive_id") {
            config.drive_id = value;
        } else if (key == "application_id") {
            config.application_id = value;
        } else if (key == "azure_tenant_id") {
            config.azure_tenant_id = value;
        } else if (key == "auth_endpoint") {
            config.auth_endpoint = value;
        } else if (key == "auth_scope") {
            config.auth_scope = value;
        } else if (key == "graph_maximum_throttle_retries") {
            const auto retries = parse_unsigned(value, key, line_number);
            if (retries > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error(
                    "value for '" + key + "' is too large at config line " +
                    std::to_string(line_number)
                );
            }
            config.graph_maximum_throttle_retries =
                static_cast<std::size_t>(retries);
        } else if (key == "graph_initial_throttle_delay_seconds") {
            config.graph_initial_throttle_delay =
                parse_seconds(value, key, line_number);
        } else if (key == "graph_maximum_throttle_delay_seconds") {
            config.graph_maximum_throttle_delay =
                parse_seconds(value, key, line_number);
        } else if (key == "dry_run") {
            config.dry_run = parse_bool(value, line_number);
        } else {
            throw std::runtime_error(
                "unknown key '" + key + "' at config line " + std::to_string(line_number)
            );
        }
    }
    if (config.graph_maximum_throttle_delay <
        config.graph_initial_throttle_delay) {
        throw std::runtime_error(
            "graph_maximum_throttle_delay_seconds must be greater than or equal "
            "to graph_initial_throttle_delay_seconds"
        );
    }
    spdlog::debug("Configuration loaded and validated");
    return config;
}

}  // namespace onedrive::config
