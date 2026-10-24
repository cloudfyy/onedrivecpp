#include "support/progress.hpp"
#include "sync/download/integrity.hpp"
#include "onedrive/util/sha256.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using onedrive::test::TemporaryDirectory;
using onedrive::test::fail;
using onedrive::test::write_file;

onedrive::graph::RemoteItem remote_item(onedrive::util::FileHash hash) {
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

int test_integrity_verification() {
    namespace detail = onedrive::sync::detail;

    const std::string sha256{
        "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7"
    };
    detail::verify_download_integrity(
        remote_item({
            .algorithm = onedrive::util::FileHashAlgorithm::sha256,
            .value =
                "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
        }),
        {
            .sha256 = sha256,
            .quick_xor = {},
        }
    );
    detail::verify_download_integrity(
        remote_item({
            .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
            .value = "ZAgDHcIAAAAAAAAABAAAAAAAAAA=",
        }),
        {
            .sha256 = sha256,
            .quick_xor = "ZAgDHcIAAAAAAAAABAAAAAAAAAA=",
        }
    );

    try {
        detail::verify_download_integrity(
            remote_item({
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value =
                    "0000000000000000000000000000000000000000000000000000000000000000",
            }),
            {
                .sha256 = sha256,
                .quick_xor = {},
            }
        );
        return fail("mismatched SHA-256 was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }

    try {
        detail::verify_download_integrity(
            remote_item({
                .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
                .value = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            }),
            {
                .sha256 = sha256,
                .quick_xor = "ZAgDHcIAAAAAAAAABAAAAAAAAAA=",
            }
        );
        return fail("mismatched QuickXorHash was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }

    onedrive::graph::RemoteItem without_hash{};
    without_hash.remote_path = "missing.bin";
    detail::verify_download_integrity(
        without_hash,
        {
            .sha256 = {},
            .quick_xor = {},
        }
    );
    return EXIT_SUCCESS;
}

int test_streaming_hashes() {
    namespace detail = onedrive::sync::detail;

    constexpr std::string_view contents{"data"};
    const auto bytes = std::as_bytes(std::span{contents});
    detail::StreamingDownloadHasher split;
    split.update(0, bytes.first(2));
    split.update(2, bytes.subspan(2));
    const auto hashes = split.finish(contents.size());
    if (!hashes.has_value() ||
        hashes->sha256 !=
            "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7" ||
        hashes->quick_xor != "ZAgDHcIAAAAAAAAABAAAAAAAAAA=") {
        return fail("split streamed hashes did not match reference values");
    }

    detail::StreamingDownloadHasher restarted;
    constexpr std::string_view discarded{"bad"};
    restarted.update(0, std::as_bytes(std::span{discarded}));
    restarted.update(0, bytes);
    const auto restarted_hashes = restarted.finish(contents.size());
    if (!restarted_hashes.has_value() ||
        restarted_hashes->sha256 != hashes->sha256 ||
        restarted_hashes->quick_xor != hashes->quick_xor) {
        return fail("streamed hashes did not reset for a restarted transfer");
    }

    detail::StreamingDownloadHasher discontinuous;
    discontinuous.update(0, bytes.first(2));
    discontinuous.update(3, bytes.subspan(2));
    if (discontinuous.finish(contents.size()).has_value()) {
        return fail("discontinuous streamed hashes were accepted");
    }

    detail::StreamingDownloadHasher wrong_size;
    wrong_size.update(0, bytes);
    if (wrong_size.finish(contents.size() + 1).has_value()) {
        return fail("incomplete streamed hashes were accepted");
    }

    detail::StreamingDownloadHasher empty;
    const auto empty_hashes = empty.finish(0);
    if (!empty_hashes.has_value() ||
        empty_hashes->sha256 !=
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
        empty_hashes->quick_xor !=
            "AAAAAAAAAAAAAAAAAAAAAAAAAAA=") {
        return fail("empty streamed hashes did not match reference values");
    }
    try {
        split.update(0, bytes);
        return fail("finalized streamed hashes accepted more data");
    } catch (const std::logic_error&) {
    }
    try {
        static_cast<void>(split.finish(contents.size()));
        return fail("streamed hashes were finalized more than once");
    } catch (const std::logic_error&) {
    }
    return EXIT_SUCCESS;
}

int test_shared_sha256_hasher() {
    constexpr std::string_view contents{"data"};
    const auto bytes = std::as_bytes(std::span{contents});
    onedrive::util::Sha256Hasher hasher;
    hasher.update(bytes.first(2));
    hasher.update(bytes.subspan(2));
    if (hasher.finish_hex() != onedrive::util::sha256_hex(contents)) {
        return fail("shared SHA-256 hasher did not preserve split updates");
    }
    try {
        hasher.update(bytes);
        return fail("finalized shared SHA-256 hasher accepted more data");
    } catch (const std::logic_error&) {
    }
    try {
        static_cast<void>(hasher.finish_hex());
        return fail("shared SHA-256 hasher was finalized more than once");
    } catch (const std::logic_error&) {
    }
    return EXIT_SUCCESS;
}

int test_sha1_hashes(const std::filesystem::path& root) {
    namespace detail = onedrive::sync::detail;
    const auto path = root / "sha1.bin";
    for (const auto& [contents, expected] :
         std::vector<std::pair<std::string, std::string>>{
             {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
             {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
             {"data", "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"},
         }) {
        write_file(path, contents);
        if (detail::sha1_hash(path) != expected ||
            detail::file_content_hash(
                path, onedrive::util::FileHashAlgorithm::sha1
            ) != expected) {
            return fail("SHA-1 file hashing did not match reference vectors");
        }
    }
    std::string binary;
    for (unsigned index = 0; index != 256U * 513U; ++index) {
        binary.push_back(static_cast<char>(index % 256U));
    }
    write_file(path, binary);
    if (detail::sha1_hash(path) != "ee1c183efc7084be47e411bec6d42f8c749da7b6") {
        return fail(
            "SHA-1 hashing failed across binary read-buffer boundaries"
        );
    }
    auto item = remote_item({
        .algorithm = onedrive::util::FileHashAlgorithm::sha1,
        .value = "A17C9AAA61E80A1BF71D0D850AF4E5BAA9800BBD",
    });
    detail::verify_download_integrity(
        item,
        {
            .sha1 = "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd",
        }
    );
    try {
        detail::verify_download_integrity(item, {.sha1 = std::string(40, '0')});
        return fail("SHA-1 verification accepted mismatched content");
    } catch (const detail::DownloadIntegrityError& error) {
        if (!std::string_view{error.what()}.contains(
                "SHA-1 verification failed"
            )) {
            return fail("SHA-1 verification used the wrong algorithm label");
        }
    }
    try {
        static_cast<void>(detail::sha1_hash(root / "missing-sha1.bin"));
        return fail("SHA-1 hashing accepted a missing file");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    onedrive::test::ScenarioProgress progress{5};
    TemporaryDirectory temporary;
    if (progress.run("sha1_hashes", [&] {
            return test_sha1_hashes(temporary.path());
        }) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = progress.run(
            "quick_xor_hash_vectors",
            [&] { return test_quick_xor_hash_vectors(temporary.path()); }
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("streaming_hashes", test_streaming_hashes);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            progress.run("shared_sha256_hasher", test_shared_sha256_hasher);
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(
        progress.run("integrity_verification", test_integrity_verification)
    );
}
