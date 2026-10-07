#pragma once

#include "onedrive/cli/event.hpp"

namespace onedrive::cli {

class ConsoleBackend {
public:
    virtual ~ConsoleBackend() = default;

    virtual void emit(const ConsoleEvent& event) = 0;
    [[nodiscard]] virtual bool confirm(
        const ConfirmationRequest& request
    ) = 0;
    [[nodiscard]] virtual OutputMode output_mode() const noexcept = 0;
};

}  // namespace onedrive::cli
