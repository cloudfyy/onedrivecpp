#pragma once

#include <termios.h>

namespace onedrive::monitor::detail {

class KeyboardInput final {
public:
    KeyboardInput(int descriptor, bool configure_terminal);
    ~KeyboardInput();

    KeyboardInput(const KeyboardInput&) = delete;
    KeyboardInput& operator=(const KeyboardInput&) = delete;
    KeyboardInput(KeyboardInput&&) = delete;
    KeyboardInput& operator=(KeyboardInput&&) = delete;

    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] bool exit_requested() const;

private:
    int descriptor_{-1};
    termios original_{};
    bool configured_{false};
};

}  // namespace onedrive::monitor::detail
