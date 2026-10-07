#pragma once

#include "onedrive/util/progress.hpp"

#include <gsl/pointers>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::cli {

enum class ColorMode {
    automatic,
    always,
    never,
};

enum class OutputMode {
    text,
    json,
};

enum class MessageKind {
    information,
    success,
    warning,
    error,
};

struct ConsoleOptions {
    ColorMode color{ColorMode::automatic};
    OutputMode output{OutputMode::text};
    bool quiet{false};
};

struct Field {
    std::string label;
    std::string key;
    std::string value;
};

struct DownloadProgressMetrics {
    std::uint64_t bytes_per_second{0};
    std::optional<std::uint64_t> estimated_seconds_remaining;
    std::uint64_t elapsed_milliseconds{0};
};

struct DeltaSummary {
    std::size_t pages{0};
    std::size_t scanned_items{0};
    std::size_t unique_changes{0};
    std::size_t files{0};
    std::size_t directories{0};
    std::size_t deletions{0};
};

[[nodiscard]] unsigned download_progress_percentage(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state
) noexcept;

class Console {
public:
    explicit Console(
        ConsoleOptions options = {},
        std::ostream& output = default_output(),
        std::ostream& error = default_error()
    );

    void message(
        MessageKind kind,
        std::string_view event,
        std::string_view text
    ) const;
    void section(
        std::string_view event,
        std::string_view title,
        const std::vector<Field>& fields
    ) const;
    void delta_progress(
        std::size_t pages,
        std::size_t items,
        util::ProgressState state
    ) const;
    void delta_summary(const DeltaSummary& summary) const;
    void blocked_item(
        std::string_view path,
        std::string_view reason_code,
        std::string_view reason_message
    ) const;
    void download_progress(
        std::size_t completed_files,
        std::size_t file_count,
        std::uint64_t downloaded,
        std::uint64_t total,
        util::ProgressState state,
        const DownloadProgressMetrics& metrics = {}
    ) const;
    void end_download_progress() const;
    [[nodiscard]] bool confirm(
        std::string_view event,
        std::string_view prompt,
        std::string_view expected
    ) const;
    [[nodiscard]] OutputMode output_mode() const noexcept;

    [[nodiscard]] static ColorMode parse_color_mode(std::string_view value);
    [[nodiscard]] static OutputMode parse_output_mode(std::string_view value);

private:
    [[nodiscard]] static std::ostream& default_output();
    [[nodiscard]] static std::ostream& default_error();

    ConsoleOptions options_;
    gsl::not_null<std::ostream*> output_;
    gsl::not_null<std::ostream*> error_;
    bool styled_{false};
    bool interactive_{false};
    mutable std::size_t delta_progress_pages_{0};
    mutable bool delta_progress_active_{false};
    mutable bool download_progress_active_{false};
};

}  // namespace onedrive::cli
