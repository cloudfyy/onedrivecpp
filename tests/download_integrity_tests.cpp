#include "download_integrity.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using onedrive::test::TemporaryDirectory;

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

void write_file(
    const std::filesystem::path& path,
    std::string_view contents
) {
    std::ofstream output{path, std::ios::binary};
    output.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size())
    );
}

onedrive::graph::RemoteItem remote_item(onedrive::FileHash hash) {
    return {
        .id = "item",
        .name = "content.bin",
        .etag = "etag",
        .parent_id = "root",
        .remote_path = "content.bin",
        .last_modified = {},
        .size = 0,
        .directory = false,
        .deleted = false,
        .root = false,
        .content_hash = std::move(hash),
    };
}

int test_quick_xor_hash_vectors(const std::filesystem::path& root) {
    namespace detail = onedrive::sync::detail;

    const auto empty = root / "empty.bin";
    write_file(empty, {});
    if (detail::quick_xor_hash(empty) !=
        "AAAAAAAAAAAAAAAAAAAAAAAAAAA=") {
        return fail("empty QuickXorHash did not match the reference vector");
    }

    const auto single = root / "single.bin";
    write_file(single, "J");
    if (detail::quick_xor_hash(single) !=
        "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
        return fail("single-byte QuickXorHash did not match the reference vector");
    }

    const auto wrapped = root / "wrapped.bin";
    constexpr std::string_view wrapped_bytes{
        "\x4f\xa2\xd8\x24\x87\xc3\x87\xcd\x49\xac"
        "\x02\xb7\xd3\xd1\xf6\x24\xc2\x53\x5e\x2b",
        20
    };
    write_file(wrapped, wrapped_bytes);
    if (detail::quick_xor_hash(wrapped) !=
        "zBTHrspn3mEcohlJdIUAbjGNaNg=") {
        return fail("wrapped QuickXorHash did not match the reference vector");
    }
    return EXIT_SUCCESS;
}

int test_integrity_verification(const std::filesystem::path& root) {
    namespace detail = onedrive::sync::detail;

    const auto path = root / "content.bin";
    write_file(path, "data");
    const std::string sha256{
        "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7"
    };
    detail::verify_download_integrity(
        path,
        remote_item({
            .algorithm = onedrive::FileHashAlgorithm::sha256,
            .value =
                "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
        }),
        sha256
    );
    detail::verify_download_integrity(
        path,
        remote_item({
            .algorithm = onedrive::FileHashAlgorithm::quick_xor,
            .value = "ZAgDHcIAAAAAAAAABAAAAAAAAAA=",
        }),
        sha256
    );

    try {
        detail::verify_download_integrity(
            path,
            remote_item({
                .algorithm = onedrive::FileHashAlgorithm::sha256,
                .value =
                    "0000000000000000000000000000000000000000000000000000000000000000",
            }),
            sha256
        );
        return fail("mismatched SHA-256 was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }

    try {
        detail::verify_download_integrity(
            path,
            remote_item({
                .algorithm = onedrive::FileHashAlgorithm::quick_xor,
                .value = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            }),
            sha256
        );
        return fail("mismatched QuickXorHash was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }

    onedrive::graph::RemoteItem without_hash{};
    without_hash.remote_path = "missing.bin";
    detail::verify_download_integrity(
        root / "missing.bin",
        without_hash,
        {}
    );
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    if (const int result = test_quick_xor_hash_vectors(temporary.path());
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_integrity_verification(temporary.path());
}
