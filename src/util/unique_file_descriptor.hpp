#pragma once

#include <unistd.h>
#include <utility>

namespace onedrive::util {

class UniqueFD final {
public:
    UniqueFD() = default;

    explicit UniqueFD(int descriptor) noexcept
        : descriptor_{descriptor} {}

    ~UniqueFD() {
        reset();
    }

    UniqueFD(const UniqueFD&) = delete;
    UniqueFD& operator=(const UniqueFD&) = delete;

    UniqueFD(UniqueFD&& other) noexcept
        : descriptor_{other.release()} {}

    UniqueFD& operator=(UniqueFD&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ != -1;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ == descriptor) {
            return;
        }
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_{-1};
};

}  // namespace onedrive::util
