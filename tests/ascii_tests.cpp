#include "util/ascii.hpp"
#include "util/uri.hpp"
#include "test_support.hpp"

#include <cstdlib>

int main() {
    if (!onedrive::util::ascii_iequals("Content-Range", "content-range") ||
        onedrive::util::ascii_iequals("Content", "Contents") ||
        onedrive::util::ascii_iequals("\xc4", "\xe4")) {
        return onedrive::test::fail(
            "ASCII case-insensitive comparison produced the wrong result"
        );
    }
    if (onedrive::util::trim_ascii_whitespace(" \t value\r\n") !=
            "value" ||
        !onedrive::util::trim_ascii_whitespace(" \r\n\t").empty() ||
        onedrive::util::trim_ascii_whitespace("value") != "value") {
        return onedrive::test::fail(
            "ASCII whitespace trimming produced the wrong result"
        );
    }
    if (onedrive::util::percent_encode_uri_component(
            "AZaz09-._~"
        ) != "AZaz09-._~" ||
        onedrive::util::percent_encode_uri_component(
            " /:#"
        ) != "%20%2F%3A%23" ||
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
