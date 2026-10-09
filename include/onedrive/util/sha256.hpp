#pragma once

#include <cstddef>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace onedrive::util {

class Sha256Hasher final {
public:
    Sha256Hasher();
    ~Sha256Hasher();

    Sha256Hasher(Sha256Hasher&&) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&&) noexcept;

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void update(std::span<const std::byte> data);
    [[nodiscard]] std::string finish_hex();
    [[nodiscard]] std::array<unsigned char, 32> finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string sha256_hex(std::string_view value);
[[nodiscard]] std::array<unsigned char, 32> sha256(std::string_view value);

}  // namespace onedrive::util
