#include "onedrive/util/sha256.hpp"
#include "util/base64.hpp"
#include "util/uri.hpp"
#include "support/common.hpp"

#include <array>

namespace {

int test_encodings() {
    using namespace onedrive;
    for (const auto& [input, expected] : std::array{
             std::pair{"", ""},
             std::pair{"f", "Zg=="},
             std::pair{"fo", "Zm8="},
             std::pair{"foo", "Zm9v"},
             std::pair{"foob", "Zm9vYg=="},
             std::pair{"fooba", "Zm9vYmE="},
             std::pair{"foobar", "Zm9vYmFy"},
         }) {
        const std::string value{input};
        const auto bytes = std::span{
            reinterpret_cast<const unsigned char*>(value.data()), value.size()
        };
        auto unpadded = std::string{expected};
        while (unpadded.ends_with('='))
            unpadded.pop_back();
        if (util::base64_encode(bytes) != expected ||
            util::base64url_encode(bytes) != unpadded) {
            return test::fail("Base64 padding or length was incorrect");
        }
    }
    const std::array<unsigned char, 3> binary{0xfb, 0xff, 0xff};
    if (util::base64_encode(binary) != "+///" ||
        util::base64url_encode(binary) != "-___") {
        return test::fail("Base64 alphabet conversion was incorrect");
    }
    const std::array<std::pair<std::string_view, std::string_view>, 3>
        parameters{{
            {"client id", "a+b &="},
            {"redirect_uri", "http://localhost:1234/"},
            {"empty", ""},
        }};
    if (!util::encode_uri_parameters({}).empty() ||
        util::encode_uri_parameters(parameters) !=
            "client%20id=a%2Bb%20%26%3D&redirect_uri=http%3A%2F%2Flocalhost%"
            "3A1234%2F&empty=") {
        return test::fail("shared URI parameter encoding was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_sha256() {
    using namespace onedrive;
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    if (util::base64url_encode(util::sha256(verifier)) !=
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM" ||
        util::sha256_hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a3961"
                                   "77a9cb410ff61f20015ad" ||
        util::sha256_hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b93"
                                "4ca495991b7852b855") {
        return test::fail("SHA-256 output or RFC 7636 challenge was incorrect");
    }
    util::Sha256Hasher hasher;
    hasher.update(std::as_bytes(std::span{verifier}));
    if (hasher.finish() != util::sha256(verifier) ||
        !test::throws_with<std::logic_error>(
            [&] { static_cast<void>(hasher.finish_hex()); }, "more than once"
        ) ||
        !test::throws_with<std::logic_error>(
            [&] { hasher.update({}); }, "finalized"
        )) {
        return test::fail(
            "binary SHA-256 did not preserve finalization guards"
        );
    }
    util::Sha256Hasher hex_hasher;
    static_cast<void>(hex_hasher.finish_hex());
    if (!test::throws_with<std::logic_error>(
            [&] { static_cast<void>(hex_hasher.finish()); }, "more than once"
        )) {
        return test::fail("hex SHA-256 allowed a second binary finalization");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_encodings() != EXIT_SUCCESS || test_sha256() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
