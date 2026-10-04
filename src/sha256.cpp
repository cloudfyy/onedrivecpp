#include "onedrive/sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <format>
#include <memory>
#include <stdexcept>

namespace onedrive {

std::string sha256_hex(std::string_view value) {
    struct DigestContextDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept {
            EVP_MD_CTX_free(context);
        }
    };
    const std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{
        EVP_MD_CTX_new()
    };
    if (!context ||
        EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("cannot initialize SHA-256 fingerprint");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
        digest_size != 32) {
        throw std::runtime_error("cannot finalize SHA-256 fingerprint");
    }

    std::string result;
    result.reserve(64);
    for (unsigned int index = 0; index < digest_size; ++index) {
        result += std::format("{:02x}", digest[index]);
    }
    return result;
}

}  // namespace onedrive
