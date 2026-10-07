#pragma once

#include "onedrive/cli/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace onedrive::cli {

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
    explicit Console(std::unique_ptr<ConsoleBackend> backend);
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
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
    [[nodiscard]] UiMode ui_mode() const noexcept;

    [[nodiscard]] static ColorMode parse_color_mode(std::string_view value);
    [[nodiscard]] static OutputMode parse_output_mode(std::string_view value);
    [[nodiscard]] static UiMode parse_ui_mode(std::string_view value);

private:
    [[nodiscard]] static std::ostream& default_output();
    [[nodiscard]] static std::ostream& default_error();

    std::unique_ptr<ConsoleBackend> backend_;
    mutable std::mutex backend_mutex_;
};

}  // namespace onedrive::cli
