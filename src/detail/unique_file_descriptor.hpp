#pragma once

#include <unistd.h>
#include <utility>

namespace onedrive::detail {

class UniqueFileDescriptor final {
public:
    UniqueFileDescriptor() = default;

    explicit UniqueFileDescriptor(int descriptor) noexcept
        : descriptor_{descriptor} {}

    ~UniqueFileDescriptor() {
        reset();
    }

    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;

    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
        : descriptor_{other.release()} {}

    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept {
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

}  // namespace onedrive::detail
