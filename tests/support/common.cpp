#include "support/common.hpp"
#include "support/progress.hpp"

#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>
#include <utility>

namespace {

using onedrive::test::fail;
using onedrive::test::throws_with;

int test_exception_matching() {
    if (!throws_with(
            [] { throw std::runtime_error("prefix: expected failure"); },
            "expected"
        ) ||
        !throws_with([] { throw std::logic_error("any standard exception"); }) ||
        throws_with([] {}, "expected") ||
        throws_with([] {}) ||
        throws_with([] { throw std::runtime_error("different"); }, "expected") ||
        !throws_with<std::runtime_error>(
            [] { throw std::overflow_error("derived exception"); }, "derived"
        )) {
        return fail("exception helper changed type or substring matching");
    }
    try {
        static_cast<void>(throws_with<std::runtime_error>(
            [] { throw std::logic_error("unexpected type"); }
        ));
        return fail("exception helper swallowed an unexpected exception type");
    } catch (const std::logic_error& error) {
        if (std::string_view{error.what()} != "unexpected type") {
            return fail("exception helper changed the propagated exception");
        }
    }
    try {
        static_cast<void>(throws_with([] { throw 42; }));
        return fail("exception helper swallowed a non-standard exception");
    } catch (int value) {
        if (value != 42) {
            return fail("exception helper changed a non-standard exception");
        }
    }
    return EXIT_SUCCESS;
}

int test_borrowed_arguments() {
    int calls = 0;
    auto operation = [owned = std::make_unique<int>(7), &calls] {
        ++calls;
        if (*owned != 7) {
            throw std::logic_error("invalid callable state");
        }
        throw std::runtime_error("expected");
    };
    const std::string message = "xexpectedy";
    const auto expected = std::string_view{message}.substr(1, 8);
    if (!throws_with(operation, expected) ||
        !throws_with(std::ref(operation), expected) ||
        !throws_with(std::move(operation), expected) ||
        calls != 3) {
        return fail("exception helper copied or reinvoked its callable");
    }
    return EXIT_SUCCESS;
}

int test_fixture_files() {
    const onedrive::test::TemporaryDirectory temporary;
    const auto file = temporary.path() / "fixture";
    const std::string contents{"a\0b", 3};
    onedrive::test::write_file(file, contents);
    if (onedrive::test::read_file(file) != contents) {
        return fail("shared fixture I/O changed binary contents");
    }
    onedrive::test::write_file(file, "");
    if (!onedrive::test::read_file(file).empty()) {
        return fail("shared fixture writer did not truncate the previous file");
    }
    const auto missing = temporary.path() / "missing" / "fixture";
    if (!throws_with<std::runtime_error>(
            [&] { onedrive::test::write_file(missing, "data"); },
            "cannot open test file for writing"
        ) ||
        !throws_with<std::runtime_error>(
            [&] { static_cast<void>(onedrive::test::read_file(missing)); },
            "cannot open test file for reading"
        )) {
        return fail("shared fixture I/O did not report file-open errors");
    }
    return EXIT_SUCCESS;
}

int test_progress_output_and_flushing() {
    struct Buffer final : std::stringbuf {
        int flushes{0};
        int sync() override {
            ++flushes;
            return std::stringbuf::sync();
        }
    } buffer;
    std::ostream output{&buffer};
    output << std::hex << std::scientific;
    const auto flags = output.flags();
    onedrive::test::ScenarioProgress progress{2, output};
    int calls = 0;
    progress.run("first", [&] {
        ++calls;
        if (buffer.str() != "[1/2] RUNNING first\n" || buffer.flushes != 1) {
            throw std::logic_error{
                "progress did not flush RUNNING before invoking the scenario"
            };
        }
        output << "application progress: .";
    });
    const int result =
        progress.run("second", [owned = std::make_unique<int>(7), &calls] {
            ++calls;
            return *owned == 7 ? EXIT_SUCCESS : EXIT_FAILURE;
        });
    const std::regex expected{
        R"(\[1/2\] RUNNING first\napplication progress: \.\n)"
        R"(\[1/2\] PASS first \([0-9]+\.[0-9]{3} ms\)\n)"
        R"(\[2/2\] RUNNING second\n\n)"
        R"(\[2/2\] PASS second \([0-9]+\.[0-9]{3} ms\)\n)"
    };
    if (result != EXIT_SUCCESS || progress.finish() != EXIT_SUCCESS ||
        calls != 2 || buffer.flushes != 4 || output.flags() != flags ||
        !std::regex_match(buffer.str(), expected)) {
        return fail(
            "scenario progress changed invocation, counts, timing or formatting"
        );
    }
    return EXIT_SUCCESS;
}

int test_progress_failures_and_exceptions() {
    std::ostringstream output;
    onedrive::test::ScenarioProgress progress{3, output};
    if (progress.run("returned failure", [] { return 23; }) != 23) {
        return fail("scenario progress replaced a failure exit code");
    }
    struct ScenarioError final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    if (!throws_with<ScenarioError>(
            [&] {
                progress.run("thrown failure", [] {
                    throw ScenarioError{"original scenario failure"};
                });
            },
            "original scenario failure"
        )) {
        return fail("scenario progress swallowed or replaced an exception");
    }
    try {
        progress.run("non-standard failure", [] { throw 42; });
        return fail("scenario progress swallowed a non-standard exception");
    } catch (int value) {
        if (value != 42) {
            return fail("scenario progress replaced a non-standard exception");
        }
    }
    const auto reported = output.str();
    if (progress.finish() != 23 || progress.finish(17) != 17 ||
        !reported.contains("[1/3] FAIL returned failure (") ||
        !reported.contains("[2/3] FAIL thrown failure (") ||
        !reported.contains(": original scenario failure\n") ||
        !reported.contains("[3/3] FAIL non-standard failure (") ||
        !reported.contains(": non-standard exception\n") ||
        reported.contains("PASS")) {
        return fail(
            "scenario progress lost the original failure or its status"
        );
    }
    return EXIT_SUCCESS;
}

int test_progress_counts_and_output_errors() {
    std::ostringstream output;
    if (!throws_with<std::invalid_argument>(
            [&] { onedrive::test::ScenarioProgress progress{0, output}; },
            "count must be positive"
        )) {
        return fail("scenario progress accepted an empty scenario plan");
    }
    onedrive::test::ScenarioProgress incomplete{2, output};
    incomplete.run("first", [] {});
    if (!throws_with<std::logic_error>(
            [&] { static_cast<void>(incomplete.finish()); },
            "before all scenarios"
        ) ||
        incomplete.finish(17) != 17) {
        return fail("scenario progress invented completion or masked failure");
    }
    onedrive::test::ScenarioProgress complete{1, output};
    complete.run("only", [] {});
    bool extra_invoked = false;
    if (!throws_with<std::logic_error>(
            [&] { complete.run("extra", [&] { extra_invoked = true; }); },
            "more test scenarios"
        ) ||
        extra_invoked || output.str().contains("extra")) {
        return fail("scenario progress ran beyond its declared scenario count");
    }
    output.setstate(std::ios::badbit);
    onedrive::test::ScenarioProgress broken{1, output};
    bool unreported_invoked = false;
    if (!throws_with<std::runtime_error>(
            [&] {
                broken.run("unreported", [&] { unreported_invoked = true; });
            },
            "cannot write test scenario progress"
        ) ||
        unreported_invoked) {
        return fail("scenario progress silently ignored an output failure");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const auto result = test_exception_matching(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_borrowed_arguments(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_fixture_files(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_progress_output_and_flushing();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_progress_failures_and_exceptions();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_progress_counts_and_output_errors();
}
