#include "support/common.hpp"

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

int main() {
    const auto message =
        fmt::format(FMT_STRING("{} [{}({})]"), "failure", "source.cpp", 42);
    if (message != "failure [source.cpp(42)]" ||
        fmt::format(FMT_STRING("{:02}"), 7) != "07" ||
        fmt::format("{:.1f} {}", 1.25, "KiB") != "1.2 KiB") {
        return onedrive::test::fail("fmt compatibility formatting mismatch");
    }

    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    spdlog::logger logger("fmt-compatibility", sink);
    logger.set_pattern("%v");
    logger.info("{}: {:02}", message, 7);
    logger.flush();
    if (output.str() != "failure [source.cpp(42)]: 07\n") {
        return onedrive::test::fail("spdlog compatibility formatting mismatch");
    }

    if (!onedrive::test::throws_with<fmt::format_error>(
            [] {
                static_cast<void>(
                    fmt::format(fmt::runtime("{:d}"), "not an integer")
                );
            },
            "invalid format specifier"
        )) {
        return onedrive::test::fail("fmt must report invalid runtime formats");
    }
    return EXIT_SUCCESS;
}
