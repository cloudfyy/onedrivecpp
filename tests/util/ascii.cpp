#include "support/progress.hpp"
#include "util/ascii.hpp"
#include "util/uri.hpp"
#include "support/common.hpp"

#include <cstdlib>

namespace {

int test_ascii_and_uri_helpers() {
    using onedrive::util::ascii_icontains;
    static_assert(ascii_icontains("Content-Range", "RANGE"));
    static_assert(ascii_icontains("", ""));
    if (!ascii_icontains("Documents/notes.one", "DOCUMENTS") ||
        !ascii_icontains("Documents/notes.one", "S/NOTES") ||
        !ascii_icontains("Documents/notes.one", ".ONE") ||
        !ascii_icontains("text", "") ||
        !ascii_icontains("\xc3\xa9", "\xc3\xa9") ||
        !ascii_icontains(
            std::string_view{"a\0B", 3}, std::string_view{"\0b", 2}
        ) ||
        ascii_icontains("", "text") ||
        ascii_icontains("short", "longer needle") ||
        ascii_icontains("Content", "none") || ascii_icontains("\xc4", "\xe4")) {
        return onedrive::test::fail(
            "ASCII substring matching changed boundaries or non-ASCII bytes"
        );
    }
    if (!onedrive::util::ascii_iequals("Content-Range", "content-range") ||
        onedrive::util::ascii_iequals("Content", "Contents") ||
        onedrive::util::ascii_iequals("\xc4", "\xe4")) {
        return onedrive::test::fail(
            "ASCII case-insensitive comparison produced the wrong result"
        );
    }
    if (onedrive::util::trim_ascii_whitespace(" \t value\r\n") != "value" ||
        !onedrive::util::trim_ascii_whitespace(" \r\n\t").empty() ||
        onedrive::util::trim_ascii_whitespace("value") != "value") {
        return onedrive::test::fail(
            "ASCII whitespace trimming produced the wrong result"
        );
    }
    if (onedrive::util::percent_encode_uri_component("AZaz09-._~") !=
            "AZaz09-._~" ||
        onedrive::util::percent_encode_uri_component(" /:#") !=
            "%20%2F%3A%23" ||
        onedrive::util::percent_encode_uri_component(
            std::string_view{"\0\xc3\xa9", 3}
        ) != "%00%C3%A9" ||
        !onedrive::util::percent_encode_uri_component("").empty()) {
        return onedrive::test::fail(
            "URI component percent encoding produced the wrong result"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{1};
    if (const auto result =
            progress.run("ascii_and_uri_helpers", test_ascii_and_uri_helpers);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
