#pragma once

#include "onedrive/cli/console.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace onedrive::test {

inline void emit_partial_results(
    const cli::Console& console,
    std::size_t count,
    std::string_view prefix = "folder/"
) {
    for (std::size_t index = 1; index <= count; ++index) {
        const auto path =
            std::string{prefix} + "partial-" + std::to_string(index) + ".bin";
        console.section(
            "partial_download",
            "Partial download:",
            {
                {.label = "remote path:", .key = "remote_path", .value = path},
                {.label = "destination:",
                 .key = "destination",
                 .value = "/sync/" + path},
                {.label = "temporary file:",
                 .key = "temporary_path",
                 .value = "/sync/" + path + ".partial"},
                {.label = "status:",
                 .key = "status",
                 .value = index % 2 == 1 ? "resumable" : "missing"},
                {.label = "completed:",
                 .key = "completed_bytes",
                 .value = "4 B"},
                {.label = "expected:", .key = "expected_bytes", .value = "8 B"},
                {.label = "actual:",
                 .key = "actual_bytes",
                 .value = index % 2 == 1 ? "4 B" : "unavailable"},
            }
        );
    }
    if (count == 0) {
        console.message(
            cli::MessageKind::information,
            "no_partial_downloads",
            "No partial downloads are recorded."
        );
    }
    console.section(
        "partial_download_summary",
        "Partial download summary:",
        {
            {.label = "recorded:",
             .key = "recorded",
             .value = std::to_string(count)},
            {.label = "resumable:",
             .key = "resumable",
             .value = std::to_string((count + 1) / 2)},
            {.label = "invalid:",
             .key = "invalid",
             .value = std::to_string(count / 2)},
            {.label = "actual bytes:",
             .key = "actual_bytes",
             .value = std::to_string(((count + 1) / 2) * 4) + " B"},
        }
    );
}

} // namespace onedrive::test
