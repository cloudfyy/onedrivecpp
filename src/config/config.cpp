#include "onedrive/config/config.hpp"

#include <cstdlib>
#include <fstream>
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

}  // namespace

Config Config::defaults() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        throw std::runtime_error("HOME is not set");
    }

    return {
        .sync_directory = std::filesystem::path{home} / "OneDrive",
        .state_directory = std::filesystem::path{home} / ".local/state/onedrive-cpp",
    };
}

Config Config::load(const std::filesystem::path& path) {
    Config config = defaults();
    if (!std::filesystem::exists(path)) {
        return config;
    }

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
        } else if (key == "dry_run") {
            config.dry_run = parse_bool(value, line_number);
        } else {
            throw std::runtime_error(
                "unknown key '" + key + "' at config line " + std::to_string(line_number)
            );
        }
    }
    return config;
}

}  // namespace onedrive::config
