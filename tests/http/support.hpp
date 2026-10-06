#pragma once

#include <sys/stat.h>

namespace onedrive::test::http {

class ScopedUmask {
public:
    explicit ScopedUmask(mode_t value)
        : previous_{::umask(value)} {
    }

    ~ScopedUmask() {
        ::umask(previous_);
    }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

private:
    mode_t previous_;
};

} // namespace onedrive::test::http
