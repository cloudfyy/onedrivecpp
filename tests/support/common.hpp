#pragma once

#include <chrono>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace onedrive::test {

[[nodiscard]] inline int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

template <typename Exception = std::exception, typename Operation>
    requires std::derived_from<Exception, std::exception> &&
             std::invocable<Operation&>
[[nodiscard]] bool throws_with(
    Operation&& operation,
    std::string_view expected = {}
) {
    try {
        std::invoke(operation);
    } catch (const Exception& error) {
        return std::string_view{error.what()}.contains(expected);
    }
    return false;
}

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_delay = std::chrono::milliseconds{1}
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        if (retry_delay == std::chrono::milliseconds::zero()) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(retry_delay);
        }
    }
    return predicate();
}

inline void write_file(
    const std::filesystem::path& path,
    std::string_view contents
) {
    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };
    if (!output) {
        throw std::runtime_error(
            "cannot open test file for writing: " + path.string()
        );
    }
    output.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size())
    );
    output.close();
    if (!output) {
        throw std::runtime_error(
            "cannot write test file: " + path.string()
        );
    }
}

[[nodiscard]] inline std::string read_file(
    const std::filesystem::path& path
) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open test file for reading: " + path.string()
        );
    }
    std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (input.bad()) {
        throw std::runtime_error(
            "cannot read test file: " + path.string()
        );
    }
    return contents;
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
