#include "monitor/input.hpp"

#include "onedrive/util/system_error.hpp"

#include <array>
#include <cerrno>
#include <unistd.h>

namespace onedrive::monitor::detail {

KeyboardInput::KeyboardInput(
    int descriptor,
    bool configure_terminal
)
    : descriptor_{descriptor} {
    if (descriptor_ < 0 || !configure_terminal) {
        return;
    }
    if (::tcgetattr(descriptor_, &original_) == -1) {
        util::throw_errno_error("cannot inspect terminal input mode");
    }
    auto input_mode = original_;
    input_mode.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    input_mode.c_cc[VMIN] = 0;
    input_mode.c_cc[VTIME] = 0;
    if (::tcsetattr(descriptor_, TCSANOW, &input_mode) == -1) {
        util::throw_errno_error("cannot enable monitor keyboard input");
    }
    configured_ = true;
}

KeyboardInput::~KeyboardInput() {
    if (configured_) {
        static_cast<void>(
            ::tcsetattr(descriptor_, TCSANOW, &original_)
        );
    }
}

int KeyboardInput::descriptor() const noexcept {
    return descriptor_;
}

bool KeyboardInput::exit_requested() const {
    std::array<char, 64> input{};
    auto size = ::read(descriptor_, input.data(), input.size());
    while (size == -1 && errno == EINTR) {
        size = ::read(descriptor_, input.data(), input.size());
    }
    if (size == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        util::throw_errno_error("cannot read monitor keyboard input");
    }
    for (ssize_t index = 0; index < size; ++index) {
        const char key = input[static_cast<std::size_t>(index)];
        if (key == 'q' || key == 'Q' || key == '\033') {
            return true;
        }
    }
    return false;
}

}  // namespace onedrive::monitor::detail
