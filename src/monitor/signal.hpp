#pragma once

#include <signal.h>

namespace onedrive::monitor::detail {

class TerminationSignalMask final {
public:
    TerminationSignalMask();
    TerminationSignalMask(const TerminationSignalMask&) = delete;
    TerminationSignalMask& operator=(const TerminationSignalMask&) = delete;
    TerminationSignalMask(TerminationSignalMask&&) = delete;
    TerminationSignalMask& operator=(TerminationSignalMask&&) = delete;
    ~TerminationSignalMask();

    [[nodiscard]] const sigset_t& signals() const noexcept;

private:
    sigset_t signals_{};
    sigset_t previous_{};
    bool active_{false};
};

}  // namespace onedrive::monitor::detail
