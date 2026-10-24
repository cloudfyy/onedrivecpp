#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace onedrive::test {

class ScenarioProgress {
public:
    explicit ScenarioProgress(
        std::size_t total, std::ostream& output = std::clog
    )
        : total_{total},
          output_{output} {
        if (total == 0) {
            throw std::invalid_argument{"test scenario count must be positive"};
        }
    }

    ScenarioProgress(const ScenarioProgress&) = delete;
    ScenarioProgress& operator=(const ScenarioProgress&) = delete;

    template <typename Operation>
        requires std::invocable<Operation&&> &&
                 (std::same_as<std::invoke_result_t<Operation &&>, int> ||
                  std::same_as<std::invoke_result_t<Operation &&>, void>)
    std::invoke_result_t<Operation&&> run(
        std::string_view name, Operation&& operation
    ) {
        if (current_ == total_) {
            throw std::logic_error{"more test scenarios than declared"};
        }
        const auto index = ++current_;
        const auto started = Clock::now();
        report(index, name, "RUNNING");
        int result = EXIT_SUCCESS;
        try {
            if constexpr (std::same_as<
                              std::invoke_result_t<Operation&&>,
                              void>) {
                std::invoke(std::forward<Operation>(operation));
            } else {
                result = std::invoke(std::forward<Operation>(operation));
            }
        } catch (const std::exception& error) {
            record_failure(EXIT_FAILURE);
            report(index, name, "FAIL", started, error.what());
            throw;
        } catch (...) {
            record_failure(EXIT_FAILURE);
            report(index, name, "FAIL", started, "non-standard exception");
            throw;
        }
        record_failure(result);
        report(index, name, result == EXIT_SUCCESS ? "PASS" : "FAIL", started);
        if constexpr (std::same_as<std::invoke_result_t<Operation&&>, int>) {
            return result;
        }
    }

    [[nodiscard]] int finish(int result = EXIT_SUCCESS) const {
        if (result != EXIT_SUCCESS) {
            return result;
        }
        if (failure_ != EXIT_SUCCESS) {
            return failure_;
        }
        if (current_ != total_) {
            throw std::logic_error{
                "test progress finished before all scenarios ran"
            };
        }
        return EXIT_SUCCESS;
    }

private:
    using Clock = std::chrono::steady_clock;

    void record_failure(int result) {
        if (failure_ == EXIT_SUCCESS && result != EXIT_SUCCESS) {
            failure_ = result;
        }
    }

    void report(
        std::size_t index,
        std::string_view name,
        std::string_view state,
        std::optional<Clock::time_point> started = {},
        std::string_view detail = {}
    ) const {
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << '[' << index << '/' << total_ << "] " << state << ' ' << name;
        if (started) {
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     Clock::now() - *started
            )
                                     .count();
            line << " (" << std::fixed << std::setprecision(3) << elapsed
                 << " ms)";
        }
        if (!detail.empty()) {
            line << ": " << detail;
        }
        if (started) {
            // A scenario may leave a console progress line unterminated.
            output_ << '\n';
        }
        output_ << line.str() << '\n';
        output_.flush();
        if (!output_) {
            throw std::runtime_error{"cannot write test scenario progress"};
        }
    }

    std::size_t total_;
    std::ostream& output_;
    std::size_t current_{0};
    int failure_{EXIT_SUCCESS};
};

} // namespace onedrive::test
