#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
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
    void download_progress(
        std::string_view path,
        std::size_t file_index,
        std::size_t file_count,
        std::uint64_t downloaded,
        std::uint64_t total,
        bool completed
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
    std::ostream& output_;
    std::ostream& error_;
    bool styled_{false};
    bool interactive_{false};
};

}  // namespace onedrive::cli
