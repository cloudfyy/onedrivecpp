#include "download_integrity.hpp"

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <format>
#include <limits>
#include <string_view>

namespace onedrive::sync::detail {
namespace {

constexpr std::size_t quick_xor_width_bytes = 20;
constexpr std::size_t quick_xor_width_bits = quick_xor_width_bytes * 8;
constexpr std::size_t quick_xor_shift = 11;

bool equal_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_character =
            static_cast<unsigned char>(left[index]);
        const auto right_character =
            static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string quick_xor_hash(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open file for QuickXorHash: " + path.string()
        );
    }

    std::array<unsigned char, quick_xor_width_bytes> hash{};
    std::array<unsigned char, std::size_t{64} * 1024U> buffer{};
    std::uint64_t length = 0;
    std::size_t bit_offset = 0;
    while (input) {
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            if (length == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error(
                    "file is too large to calculate QuickXorHash"
                );
            }
            const auto byte_index = bit_offset / 8;
            const auto shift = bit_offset % 8;
            const auto value = buffer[static_cast<std::size_t>(index)];
            hash[byte_index] ^= static_cast<unsigned char>(value << shift);
            if (shift != 0) {
                hash[(byte_index + 1) % hash.size()] ^=
                    static_cast<unsigned char>(value >> (8 - shift));
            }
            ++length;
            bit_offset =
                (bit_offset + quick_xor_shift) % quick_xor_width_bits;
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "cannot read file for QuickXorHash: " + path.string()
        );
    }
    for (std::size_t index = 0; index < sizeof(length); ++index) {
        hash[hash.size() - sizeof(length) + index] ^=
            static_cast<unsigned char>(length >> (index * 8));
    }

    std::array<
        unsigned char,
        4 * ((quick_xor_width_bytes + 2) / 3) + 1
    > encoded{};
    const int encoded_size = EVP_EncodeBlock(
        encoded.data(),
        hash.data(),
        static_cast<int>(hash.size())
    );
    if (encoded_size <= 0) {
        throw std::runtime_error("cannot encode QuickXorHash");
    }
    return {
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<std::size_t>(encoded_size)
    };
}

void verify_download_integrity(
    const std::filesystem::path& path,
    const graph::RemoteItem& item,
    const std::string& sha256_fingerprint
) {
    if (!item.content_hash.has_value()) {
        return;
    }
    const auto& expected = item.content_hash.value();
    const std::string actual =
        expected.algorithm == FileHashAlgorithm::sha256 ?
            sha256_fingerprint :
            quick_xor_hash(path);
    const bool matches =
        expected.algorithm == FileHashAlgorithm::sha256 ?
            equal_case_insensitive(actual, expected.value) :
            actual == expected.value;
    if (!matches) {
        const std::string_view algorithm =
            expected.algorithm == FileHashAlgorithm::sha256 ?
                "SHA-256" :
                "QuickXorHash";
        throw DownloadIntegrityError(
            std::format(
                "downloaded content hash mismatch for '{}': {} verification "
                "failed",
                item.remote_path,
                algorithm
            )
        );
    }
}

}  // namespace onedrive::sync::detail
