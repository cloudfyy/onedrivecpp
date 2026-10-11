#pragma once

#include <cstddef>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace onedrive::util {

inline constexpr std::size_t sha256_digest_bytes = 32;
inline constexpr std::size_t sha256_hex_characters = sha256_digest_bytes * 2U;

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
    [[nodiscard]] std::array<unsigned char, sha256_digest_bytes> finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string sha256_hex(std::string_view value);
[[nodiscard]] std::array<unsigned char, sha256_digest_bytes>
sha256(std::string_view value);

}  // namespace onedrive::util
