#pragma once

#include <openssl/evp.h>

#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace onedrive::util {

[[nodiscard]] inline std::string
base64_encode(std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
        return {};
    }
    // Both input and encoded length must fit OpenSSL's int API.
    constexpr auto maximum =
        static_cast<std::size_t>(std::numeric_limits<int>::max() / 4) * 3;
    if (bytes.size() > maximum) {
        throw std::length_error{"input is too large for Base64 encoding"};
    }
    const auto length = 4 * ((bytes.size() + 2) / 3);
    std::string encoded(length + 1, '\0');
    const auto written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        bytes.data(),
        static_cast<int>(bytes.size())
    );
    if (written < 0 || static_cast<std::size_t>(written) != length) {
        throw std::runtime_error{"cannot encode Base64"};
    }
    encoded.resize(length);
    return encoded;
}

[[nodiscard]] inline std::string
base64url_encode(std::span<const unsigned char> bytes) {
    auto encoded = base64_encode(bytes);
    for (auto& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    return encoded;
}

} // namespace onedrive::util
