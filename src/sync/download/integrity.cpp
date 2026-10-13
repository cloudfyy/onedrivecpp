#include "sync/download/integrity.hpp"

#include "util/ascii.hpp"
#include "util/base64.hpp"
#include "onedrive/util/sha256.hpp"
#include "sync/filesystem/operations.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace onedrive::sync::detail {
namespace {

constexpr std::size_t quick_xor_width_bytes = 20;
constexpr std::size_t quick_xor_width_bits = quick_xor_width_bytes * 8;
constexpr std::size_t quick_xor_shift = 11;

class QuickXorAccumulator final {
public:
    void update(std::span<const std::byte> data) {
        for (const auto byte : data) {
            if (length_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error(
                    "file is too large to calculate QuickXorHash"
                );
            }
            const auto byte_index = bit_offset_ / 8;
            const auto shift = bit_offset_ % 8;
            const auto value = std::to_integer<unsigned char>(byte);
            hash_[byte_index] ^=
                static_cast<unsigned char>(value << shift);
            if (shift != 0) {
                hash_[(byte_index + 1) % hash_.size()] ^=
                    static_cast<unsigned char>(value >> (8 - shift));
            }
            ++length_;
            bit_offset_ =
                (bit_offset_ + quick_xor_shift) % quick_xor_width_bits;
        }
    }

    [[nodiscard]] std::string finish() const {
        auto hash = hash_;
        for (std::size_t index = 0; index < sizeof(length_); ++index) {
            hash[hash.size() - sizeof(length_) + index] ^=
                static_cast<unsigned char>(length_ >> (index * 8));
        }

        return util::base64_encode(hash);
    }

private:
    std::array<unsigned char, quick_xor_width_bytes> hash_{};
    std::uint64_t length_{0};
    std::size_t bit_offset_{0};
};

}  // namespace

class StreamingDownloadHasher::Impl final {
public:
    void update(
        std::uint64_t offset,
        std::span<const std::byte> data
    ) {
        if (finished_) {
            throw std::logic_error(
                "cannot update a finalized streamed download hash"
            );
        }
        if (offset == 0 && length_ != 0) {
            reset();
        }
        if (!valid_) {
            return;
        }
        if (offset != length_ ||
            data.size() >
                std::numeric_limits<std::uint64_t>::max() - length_) {
            valid_ = false;
            return;
        }
        sha256_.update(data);
        quick_xor_.update(data);
        length_ += static_cast<std::uint64_t>(data.size());
    }

    [[nodiscard]] std::optional<DownloadHashes> finish(
        std::uint64_t expected_size
    ) {
        if (finished_) {
            throw std::logic_error(
                "cannot finalize streamed download hashes more than once"
            );
        }
        finished_ = true;
        if (!valid_ || length_ != expected_size) {
            return std::nullopt;
        }
        return DownloadHashes{
            .sha256 = sha256_.finish_hex(),
            .quick_xor = quick_xor_.finish(),
            .sha1 = {},
        };
    }

private:
    void reset() {
        sha256_ = util::Sha256Hasher{};
        quick_xor_ = {};
        length_ = 0;
        valid_ = true;
    }

    util::Sha256Hasher sha256_;
    QuickXorAccumulator quick_xor_;
    std::uint64_t length_{0};
    bool valid_{true};
    bool finished_{false};
};

StreamingDownloadHasher::StreamingDownloadHasher()
    : impl_{std::make_unique<Impl>()} {}

StreamingDownloadHasher::~StreamingDownloadHasher() = default;

StreamingDownloadHasher::StreamingDownloadHasher(
    StreamingDownloadHasher&&
) noexcept = default;

StreamingDownloadHasher& StreamingDownloadHasher::operator=(
    StreamingDownloadHasher&&
) noexcept = default;

void StreamingDownloadHasher::update(
    std::uint64_t offset,
    std::span<const std::byte> data
) {
    impl_->update(offset, data);
}

std::optional<DownloadHashes> StreamingDownloadHasher::finish(
    std::uint64_t expected_size
) {
    return impl_->finish(expected_size);
}

std::string quick_xor_hash(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open file for QuickXorHash: " + path.string()
        );
    }

    constexpr std::size_t hash_buffer_bytes = std::size_t{64} * 1024U;
    std::array<unsigned char, hash_buffer_bytes> buffer{};
    QuickXorAccumulator hash;
    while (input) {
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );
        const auto count = input.gcount();
        hash.update(std::as_bytes(std::span{
            buffer.data(),
            static_cast<std::size_t>(count)
        }));
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "cannot read file for QuickXorHash: " + path.string()
        );
    }
    return hash.finish();
}

std::string sha1_hash(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open file for SHA-1: " + path.string()
        );
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
        EVP_MD_CTX_new(), EVP_MD_CTX_free
    };
    if (!context ||
        EVP_DigestInit_ex(context.get(), EVP_sha1(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize SHA-1 hash");
    }
    constexpr std::size_t hash_buffer_bytes = std::size_t{64} * 1024U;
    std::array<char, hash_buffer_bytes> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count != 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(), count) != 1) {
            throw std::runtime_error("cannot update SHA-1 hash");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "cannot read file for SHA-1: " + path.string()
        );
    }
    std::array<unsigned char, 20> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 ||
        size != digest.size()) {
        throw std::runtime_error("cannot finalize SHA-1 hash");
    }
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0FU]);
    }
    return result;
}

std::string file_content_hash(
    const std::filesystem::path& path, util::FileHashAlgorithm algorithm
) {
    switch (algorithm) {
    case util::FileHashAlgorithm::quick_xor:
        return quick_xor_hash(path);
    case util::FileHashAlgorithm::sha256:
        return content_fingerprint(path);
    case util::FileHashAlgorithm::sha1:
        return sha1_hash(path);
    }
    throw std::invalid_argument("unknown file hash algorithm");
}

void verify_download_integrity(
    const graph::RemoteItem& item,
    const DownloadHashes& hashes
) {
    if (!item.content_hash.has_value()) {
        return;
    }
    const auto& expected = item.content_hash.value();
    const std::string actual =
        expected.algorithm == util::FileHashAlgorithm::sha256 ? hashes.sha256
        : expected.algorithm == util::FileHashAlgorithm::sha1
            ? hashes.sha1
            : hashes.quick_xor;
    const bool matches =
        expected.algorithm != util::FileHashAlgorithm::quick_xor
            ? onedrive::util::ascii_iequals(actual, expected.value)
            : actual == expected.value;
    if (!matches) {
        const std::string_view algorithm =
            expected.algorithm == util::FileHashAlgorithm::sha256 ? "SHA-256"
            : expected.algorithm == util::FileHashAlgorithm::sha1
                ? "SHA-1"
                : "QuickXorHash";
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
