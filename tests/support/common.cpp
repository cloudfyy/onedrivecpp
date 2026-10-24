#include "support/common.hpp"
#include "support/progress.hpp"

#include <cerrno>
#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>
#include <utility>

#include <sys/wait.h>

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

int test_progress_skip_codes() {
    std::ostringstream output;
    onedrive::test::ScenarioProgress progress{5, output};
    if (progress.run(
            "skipped", [] { return 77; }, 77
        ) != 77 ||
        progress.run(
            "failed", [] { return 23; }, 77
        ) != 23 ||
        progress.run(
            "passed", [] { return EXIT_SUCCESS; }, 77
        ) != EXIT_SUCCESS ||
        !throws_with<std::runtime_error>(
            [&] {
                progress.run(
                    "thrown", [] { throw std::runtime_error{"original"}; }, 77
                );
            },
            "original"
        ) ||
        progress.run(
            "skipped after failure", [] { return 77; }, 77
        ) != 77 ||
        progress.finish() != 23 || progress.finish(19) != 19) {
        return fail("scenario progress changed skip codes or masked a failure");
    }
    const auto reported = output.str();
    if (!reported.contains("[1/5] SKIP skipped (") ||
        !reported.contains("[2/5] FAIL failed (") ||
        !reported.contains("[3/5] PASS passed (") ||
        !reported.contains("[4/5] FAIL thrown (") ||
        !reported.contains("[5/5] SKIP skipped after failure (") ||
        !reported.contains(": original\n")) {
        return fail("scenario progress confused skips, failures and success");
    }
    onedrive::test::ScenarioProgress skipped{1, output};
    if (skipped.run(
            "only skip", [] { return 77; }, 77
        ) != 77 ||
        skipped.finish() != 77 ||
        !output.str().contains("[1/1] SKIP only skip (")) {
        return fail("scenario progress replaced a configured skip exit code");
    }
    onedrive::test::ScenarioProgress incomplete{2, output};
    incomplete.run("skip before unfinished", [] { return 77; }, 77);
    if (!throws_with<std::logic_error>(
            [&] { static_cast<void>(incomplete.finish()); },
            "before all scenarios"
        )) {
        return fail("a skipped scenario hid an incomplete scenario plan");
    }
    onedrive::test::ScenarioProgress ordinary{1, output};
    if (ordinary.run("ordinary", [] { return 77; }) != 77 ||
        ordinary.finish() != 77 ||
        !output.str().contains("[1/1] FAIL ordinary (")) {
        return fail("scenario progress treated an unconfigured code as a skip");
    }
    onedrive::test::ScenarioProgress invalid{1, output};
    bool invoked = false;
    if (!throws_with<std::invalid_argument>(
            [&] { invalid.run("invalid", [&] { invoked = true; }, 0); },
            "skip code must be nonzero"
        ) ||
        invoked || output.str().contains("RUNNING invalid")) {
        return fail("scenario progress accepted success as a skip code");
    }
    invalid.run("valid", [] {});
    return invalid.finish();
}

int test_progress_forked_process() {
    const onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "progress";
    const auto parent = ::getpid();
    std::ofstream output{path};
    onedrive::test::ScenarioProgress progress{1, output};
    const int result = progress.run("forked", [&] {
        const auto child = ::fork();
        if (child == -1) {
            return fail("cannot fork scenario progress regression");
        }
        if (child == 0) {
            return EXIT_SUCCESS;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != EXIT_SUCCESS) {
            return fail("forked scenario progress did not preserve child exit");
        }
        return EXIT_SUCCESS;
    });
    if (::getpid() != parent) {
        ::_exit(progress.finish(result));
    }
    if (result != EXIT_SUCCESS || progress.finish() != EXIT_SUCCESS) {
        return fail("forked scenario progress changed the parent result");
    }
    output.close();
    const std::regex expected{
        R"(\[1/1\] RUNNING forked\n\n)"
        R"(\[1/1\] PASS forked \([0-9]+\.[0-9]{3} ms\)\n)"
    };
    if (!std::regex_match(onedrive::test::read_file(path), expected)) {
        return fail("forked child emitted its parent's scenario result");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{8};
    if (const auto result =
            progress.run("exception_matching", test_exception_matching);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("borrowed_arguments", test_borrowed_arguments);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run("fixture_files", test_fixture_files);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "progress_output_and_flushing", test_progress_output_and_flushing
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "progress_failures_and_exceptions",
            test_progress_failures_and_exceptions
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "progress_counts_and_output_errors",
            test_progress_counts_and_output_errors
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("progress_skip_codes", test_progress_skip_codes);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(
        progress.run("progress_forked_process", test_progress_forked_process)
    );
}
