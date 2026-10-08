#include "support/common.hpp"

#include <memory>
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

} // namespace

int main() {
    if (const auto result = test_exception_matching(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_borrowed_arguments(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_fixture_files();
}
