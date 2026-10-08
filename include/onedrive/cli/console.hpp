#pragma once

#include "onedrive/cli/backend.hpp"
#include "onedrive/events/observer.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace onedrive::cli {

using events::download_progress_percentage;

class Console : public events::Observer {
public:
    explicit Console(
        ConsoleOptions options = {},
        std::ostream& output = default_output(),
        std::ostream& error = default_error()
    );
    explicit Console(std::unique_ptr<ConsoleBackend> backend);
    ~Console() override;
    void finish() const;

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;
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
    [[nodiscard]] static TuiTheme parse_tui_theme(std::string_view value);

private:
    void on_event(const events::Event& event) const override;
    [[nodiscard]] static std::ostream& default_output();
    [[nodiscard]] static std::ostream& default_error();

    std::unique_ptr<ConsoleBackend> backend_;
};

}  // namespace onedrive::cli
