#include "ui/cli/backend_factory.hpp"
#include "ui/cli/app/drive_fields.hpp"
#include "ui/cli/format.hpp"
#include "onedrive/util/byte_size.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "support/common.hpp"
#include "cli/partials_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include "tui_cases.hpp"

namespace onedrive::test::tui_cases {

using onedrive::test::fail;

int test_drive_details(std::size_t rows, int pages) {
    using namespace onedrive::cli;
    std::ostringstream output;
    std::ostringstream error;
    const Console console{detail::make_ftxui_console_backend(
        {.color = ColorMode::never, .ui = UiMode::tui, .view = TuiView::drives},
        output,
        error,
        100,
        rows
    )};
    auto fields = onedrive::app::detail::drive_fields(
        {
            .id = "real-drive-id",
            .name = "OneDrive",
            .quota =
                onedrive::graph::DriveQuota{
                    .total = 5ULL * 1024 * 1024 * 1024,
                    .used = 1024,
                    .remaining = 4096,
                    .deleted = 0,
                    .state = "normal",
                },
        },
        true
    );
    fields.insert(
        fields.begin() + 2,
        {
            .label = "reference:",
            .key = "reference",
            .value = "me",
        }
    );
    fields.insert(
        fields.end(),
        {
            {.label = "statistics source:",
             .key = "statistics_source",
             .value = "local state (not cloud totals)"},
            {.label = "known files:", .key = "known_files", .value = "12"},
            {.label = "downloaded (local):",
             .key = "downloaded_files",
             .value = "8"},
            {.label = "pending downloads:",
             .key = "pending_files",
             .value = "3"},
            {.label = "blocked files:", .key = "blocked_files", .value = "1"},
            {.label = "state database:",
             .key = "state_database",
             .value = "present"},
        }
    );
    for (int page = 0; page < pages; ++page) {
        console.section("drive", "OneDrive drive:", fields);
    }
    std::istringstream input{"\n"};
    auto* original = std::cin.rdbuf(input.rdbuf());
    console.finish();
    std::cin.rdbuf(original);
    const auto rendered = output.str();
    const auto frame = rendered.substr(rendered.rfind("\033[H"));
    if (!frame.contains("Press Enter to exit") || frame.contains("CLOSE") ||
        !frame.contains("Drive 1/" + std::to_string(pages)) ||
        frame.contains("Left/Right or n/p") != (pages > 1)) {
        return fail("drive TUI hid or retained the ambiguous exit prompt");
    }
    if (rows < 30) {
        return EXIT_SUCCESS;
    }
    for (const auto* expected : {
             "reference: me",
             "drive id: real-drive-id",
             "total: 5.00 GiB",
             "used: 1.00 KiB",
             "remaining: 4.00 KiB",
             "deleted: 0 B",
             "known files: 12",
             "downloaded (local): 8",
             "pending downloads: 3",
             "blocked files: 1",
             "local state (not cloud totals)",
             "Press Enter to exit",
         }) {
        if (!frame.contains(expected)) {
            return fail(std::string{"drive TUI omitted: "} + expected);
        }
    }
    if (frame.contains("CLOSE")) {
        return fail("drive TUI retained the ambiguous exit prompt");
    }
    return EXIT_SUCCESS;
}

int test_drive_pagination() {
    using namespace onedrive::cli;
    struct Case {
        std::string_view input;
        int page;
    };
    constexpr std::array cases{
        Case{"\n", 1},
        Case{"q\n", 1},
        Case{"Q\n", 1},
        Case{"p\nq\n", 1},
        Case{"n\nq\n", 2},
        Case{"n\nn\nn\nq\n", 3},
        Case{"n\np\np\nq\n", 1},
        Case{"\033[C\nq\n", 2},
        Case{"n\n\033[D\nq\n", 1},
        Case{"x\nq\n", 1},
        Case{"", 1},
        Case{"\033[D\nq\n", 1},
        Case{"n\nn\n\033[C\nq\n", 3},
        Case{"q\nuntouched\n", 1},
    };
    for (const auto& test : cases) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::never,
             .ui = UiMode::tui,
             .view = TuiView::drives},
            output,
            error,
            100,
            12
        )};
        console.section("account", "Account:", {});
        for (int page = 1; page <= 3; ++page) {
            console.section(
                "drive",
                "OneDrive drive:",
                {
                    {.label = "name:",
                     .key = "name",
                     .value = "Drive-name-" + std::to_string(page)},
                    {.label = "drive id:",
                     .key = "drive_id",
                     .value = std::to_string(page)},
                }
            );
        }
        std::istringstream input{std::string{test.input}};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        console.finish();
        std::cin.rdbuf(original);
        if (test.input.ends_with("untouched\n") && input.peek() != 'u') {
            return fail("Drive paging read input after completion");
        }
        const auto rendered = output.str();
        const auto frame = rendered.substr(rendered.rfind("\033[H"));
        if (!frame.contains("Drive " + std::to_string(test.page) + "/3") ||
            !frame.contains("Drive-name-" + std::to_string(test.page)) ||
            !frame.contains("Left/Right or n/p") ||
            !frame.contains("Press Enter to exit (or q)")) {
            return fail(
                "Drive paging lost results or selected an incorrect page"
            );
        }
        for (int page = 1; page <= 3; ++page) {
            if (page != test.page &&
                frame.contains("Drive-name-" + std::to_string(page))) {
                return fail("Drive paging mixed details from different drives");
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_drive_field_helpers() {
    using onedrive::util::scale_bytes;
    constexpr std::array<std::string_view, 7> units{
        "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"
    };
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto bytes = std::uint64_t{1} << (10 * index);
        const auto size = scale_bytes(bytes);
        if (size.value != 1 || size.unit != units[index]) {
            return fail("shared byte scaling mishandled unit boundaries");
        }
    }
    if (scale_bytes(0).value != 0 || scale_bytes(0).unit != "B" ||
        scale_bytes(1023).value != 1023 || scale_bytes(1023).unit != "B" ||
        scale_bytes(std::numeric_limits<std::uint64_t>::max()).unit != "EiB" ||
        scale_bytes(std::numeric_limits<std::uint64_t>::max()).value != 16 ||
        onedrive::cli::detail::format_bytes(0) != "0 B" ||
        onedrive::cli::detail::format_bytes(1023) != "1023 B" ||
        onedrive::cli::detail::format_bytes(1024) != "1.0 KiB" ||
        onedrive::cli::detail::format_bytes(1ULL << 40) != "1024.0 GiB") {
        return fail("shared byte scaling changed CLI precision or unit limits");
    }
    using onedrive::app::detail::format_bytes;
    if (format_bytes(0) != "0 B" || format_bytes(1023) != "1023 B" ||
        format_bytes(1024) != "1.00 KiB" ||
        format_bytes(1ULL << 20) != "1.00 MiB" ||
        format_bytes(1ULL << 30) != "1.00 GiB" ||
        format_bytes(1ULL << 40) != "1.00 TiB" ||
        format_bytes(1ULL << 50) != "1024.00 TiB") {
        return fail("shared quota formatter mishandled unit boundaries");
    }
    const auto fields = onedrive::app::detail::drive_fields({
        .id = "site-drive",
        .name = "Library",
        .quota = std::nullopt,
    });
    if (std::ranges::any_of(
            fields,
            [](const auto& field) {
                return field.key == "configured" || field.key == "reference";
            }
        ) ||
        std::ranges::none_of(fields, [](const auto& field) {
            return field.key == "total" && field.value == "unavailable";
        })) {
        return fail("shared Drive fields invented configuration or quota data");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::tui_cases
