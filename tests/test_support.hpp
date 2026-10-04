#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

namespace onedrive::test {

[[nodiscard]] inline int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(
        const std::source_location& location = std::source_location::current()
    )
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-" +
               std::filesystem::path{location.file_name()}.stem().string() +
               "-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

}  // namespace onedrive::test
