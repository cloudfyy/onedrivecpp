#include "detail/ascii.hpp"
#include "test_support.hpp"

#include <cstdlib>

int main() {
    if (!onedrive::detail::ascii_iequals("Content-Range", "content-range") ||
        onedrive::detail::ascii_iequals("Content", "Contents") ||
        onedrive::detail::ascii_iequals("\xc4", "\xe4")) {
        return onedrive::test::fail(
            "ASCII case-insensitive comparison produced the wrong result"
        );
    }
    if (onedrive::detail::trim_ascii_whitespace(" \t value\r\n") !=
            "value" ||
        !onedrive::detail::trim_ascii_whitespace(" \r\n\t").empty() ||
        onedrive::detail::trim_ascii_whitespace("value") != "value") {
        return onedrive::test::fail(
            "ASCII whitespace trimming produced the wrong result"
        );
    }
    return EXIT_SUCCESS;
}
