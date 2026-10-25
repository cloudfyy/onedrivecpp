#pragma once

#include "onedrive/ui/cli/event.hpp"

namespace onedrive::cli {

class ConsoleBackend {
public:
    ConsoleBackend() = default;
    virtual ~ConsoleBackend() = default;
    ConsoleBackend(const ConsoleBackend&) = default;
    ConsoleBackend& operator=(const ConsoleBackend&) = default;
    ConsoleBackend(ConsoleBackend&&) = default;
    ConsoleBackend& operator=(ConsoleBackend&&) = default;

    virtual void emit(const ConsoleEvent& event) = 0;
    virtual void finish() {}
    virtual void finish_file_history() {
    }
    [[nodiscard]] virtual bool confirm(
        const ConfirmationRequest& request
    ) = 0;
    [[nodiscard]] virtual OutputMode output_mode() const noexcept = 0;
    [[nodiscard]] virtual UiMode ui_mode() const noexcept = 0;
};

}  // namespace onedrive::cli
