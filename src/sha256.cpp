#include "onedrive/sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace onedrive {

class Sha256Hasher::Impl final {
public:
    Impl() : context_{EVP_MD_CTX_new()} {
        if (!context_ ||
            EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("cannot initialize SHA-256 hash");
        }
    }

    void update(std::span<const std::byte> data) {
        if (finished_) {
            throw std::logic_error(
                "cannot update a finalized SHA-256 hash"
            );
        }
        if (!data.empty() &&
            EVP_DigestUpdate(
                context_.get(),
                data.data(),
                data.size()
            ) != 1) {
            throw std::runtime_error("cannot update SHA-256 hash");
        }
    }

    [[nodiscard]] std::string finish_hex() {
        if (finished_) {
            throw std::logic_error(
                "cannot finalize a SHA-256 hash more than once"
            );
        }
        finished_ = true;
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        if (EVP_DigestFinal_ex(
                context_.get(),
                digest.data(),
                &digest_size
            ) != 1 ||
            digest_size != 32) {
            throw std::runtime_error("cannot finalize SHA-256 hash");
        }

        std::string result;
        result.reserve(static_cast<std::size_t>(digest_size) * 2U);
        constexpr std::string_view hex{"0123456789abcdef"};
        for (unsigned int index = 0; index < digest_size; ++index) {
            result.push_back(hex[digest[index] >> 4U]);
            result.push_back(hex[digest[index] & 0x0FU]);
        }
        return result;
    }

private:
    struct DigestContextDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept {
            EVP_MD_CTX_free(context);
        }
    };

    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context_;
    bool finished_{false};
};

Sha256Hasher::Sha256Hasher() : impl_{std::make_unique<Impl>()} {}

Sha256Hasher::~Sha256Hasher() = default;

Sha256Hasher::Sha256Hasher(Sha256Hasher&&) noexcept = default;

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&&) noexcept = default;

void Sha256Hasher::update(std::span<const std::byte> data) {
    impl_->update(data);
}

std::string Sha256Hasher::finish_hex() {
    return impl_->finish_hex();
}

std::string sha256_hex(std::string_view value) {
    Sha256Hasher hasher;
    hasher.update(std::as_bytes(std::span{value}));
    return hasher.finish_hex();
}

}  // namespace onedrive
