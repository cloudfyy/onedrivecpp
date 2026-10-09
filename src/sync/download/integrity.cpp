#include "sync/download/integrity.hpp"

#include "util/ascii.hpp"
#include "util/base64.hpp"
#include "onedrive/util/sha256.hpp"

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

    std::array<unsigned char, std::size_t{64} * 1024U> buffer{};
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

void verify_download_integrity(
    const graph::RemoteItem& item,
    const DownloadHashes& hashes
) {
    if (!item.content_hash.has_value()) {
        return;
    }
    const auto& expected = item.content_hash.value();
    const std::string actual =
        expected.algorithm == util::FileHashAlgorithm::sha256 ?
            hashes.sha256 :
            hashes.quick_xor;
    const bool matches =
        expected.algorithm == util::FileHashAlgorithm::sha256 ?
            onedrive::util::ascii_iequals(actual, expected.value) :
            actual == expected.value;
    if (!matches) {
        const std::string_view algorithm =
            expected.algorithm == util::FileHashAlgorithm::sha256 ?
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
