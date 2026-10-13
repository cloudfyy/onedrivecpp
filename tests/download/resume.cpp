#include "support.hpp"
#include "onedrive/logging/logging.hpp"
#include "graph/support.hpp"

#include <algorithm>

namespace {

using namespace onedrive::test::download;

int test_resume() {
    namespace detail = onedrive::sync::detail;
    DownloadFixture fixture;
    auto& temporary = fixture.temporary;
    const auto& root = fixture.root;
    const auto& metadata = fixture.metadata;
    const auto& safe_root = fixture.safe_root;
    auto& space = fixture.space;
    auto& graph = fixture.graph;
    auto& items = fixture.items;
    graph.send_checkpoint = true;
    auto truncated_item = remote_item("truncated", "truncated.txt");
    truncated_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
    };
    const auto truncated_destination = root / "truncated.txt";
    const auto truncated_partial =
        root / ".truncated.txt.onedrive-partial-previous";
    {
        std::ofstream output{truncated_partial, std::ios::binary};
        output << "dauncommitted";
    }
    items.partials.emplace(
        "truncated",
        onedrive::storage::PartialDownload{
            .item = item_state(truncated_item, truncated_destination),
            .temporary_path = truncated_partial,
            .completed_bytes = 2,
        }
    );
    detail::DownloadSpaceCoordinator resume_space{
        root, 0, [](const std::filesystem::path&) { return std::uintmax_t{2}; }
    };
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        truncated_item,
        item_state(truncated_item, truncated_destination),
        truncated_destination,
        metadata,
        resume_space
    ));
    std::ifstream truncated_input{truncated_destination, std::ios::binary};
    const std::string truncated_contents{
        std::istreambuf_iterator<char>{truncated_input},
        std::istreambuf_iterator<char>{}
    };
    if (graph.last_initial_offset != 2 || truncated_contents != "data" ||
        items.partials.contains("truncated")) {
        return fail(
            "partial download tail was not truncated to its durable checkpoint"
        );
    }

    const auto complete_item = remote_item("complete", "complete.txt");
    const auto complete_destination = root / "complete.txt";
    const auto complete_partial =
        root / ".complete.txt.onedrive-partial-previous";
    {
        std::ofstream output{complete_partial, std::ios::binary};
        output << "data";
    }
    items.partials.emplace(
        "complete",
        onedrive::storage::PartialDownload{
            .item = item_state(complete_item, complete_destination),
            .temporary_path = complete_partial,
            .completed_bytes = 4,
        }
    );
    detail::DownloadSpaceCoordinator complete_space{
        root, 0, [](const std::filesystem::path&) -> std::uintmax_t {
            throw std::runtime_error{
                "zero-byte reservation queried filesystem capacity"
            };
        }
    };
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        complete_item,
        item_state(complete_item, complete_destination),
        complete_destination,
        metadata,
        complete_space
    ));
    if (graph.last_initial_offset != 4 ||
        !std::filesystem::exists(complete_destination) ||
        items.partials.contains("complete")) {
        return fail("fully checkpointed download was not installed");
    }

    const auto stale_item = remote_item("stale", "stale.txt");
    const auto stale_destination = root / "stale.txt";
    const auto stale_partial = root / ".stale.txt.onedrive-partial-previous";
    {
        std::ofstream output{stale_partial, std::ios::binary};
        output << "ol";
    }
    auto stale_state = item_state(stale_item, stale_destination);
    stale_state.etag = "old-etag";
    items.partials.emplace(
        "stale",
        onedrive::storage::PartialDownload{
            .item = stale_state,
            .temporary_path = stale_partial,
            .completed_bytes = 2,
        }
    );
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        stale_item,
        item_state(stale_item, stale_destination),
        stale_destination,
        metadata,
        space
    ));
    if (graph.last_initial_offset != 0 ||
        std::filesystem::exists(stale_partial) ||
        items.partials.contains("stale")) {
        return fail("stale partial download was not restarted safely");
    }

    const auto unsafe_item = remote_item("unsafe-partial", "unsafe.txt");
    const auto unsafe_destination = root / "unsafe.txt";
    const auto unsafe_partial =
        temporary.path() / ".unsafe.txt.onedrive-partial-outside";
    {
        std::ofstream output{unsafe_partial, std::ios::binary};
        output << "da";
    }
    items.partials.emplace(
        "unsafe-partial",
        onedrive::storage::PartialDownload{
            .item = item_state(unsafe_item, unsafe_destination),
            .temporary_path = unsafe_partial,
            .completed_bytes = 2,
        }
    );
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            unsafe_item,
            item_state(unsafe_item, unsafe_destination),
            unsafe_destination,
            metadata,
            space
        ));
        return fail("unsafe partial download path was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(unsafe_partial) ||
        !items.partials.contains("unsafe-partial") ||
        std::filesystem::exists(unsafe_destination)) {
        return fail("unsafe partial download path changed recovery state");
    }

    auto quick_xor_item = remote_item("quick-xor", "quick-xor.txt");
    quick_xor_item.size = 1;
    quick_xor_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
        .value = "SgAAAAAAAAAAAAAAAQAAAAAAAAA=",
    };
    const auto quick_xor_destination = root / "quick-xor.txt";
    graph.contents = "J";
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        quick_xor_item,
        item_state(quick_xor_item, quick_xor_destination),
        quick_xor_destination,
        metadata,
        space
    ));
    if (!std::filesystem::exists(quick_xor_destination) ||
        items.partials.contains("quick-xor")) {
        return fail("valid QuickXorHash download was not installed");
    }

    auto corrupt_item = remote_item("corrupt", "corrupt.txt");
    corrupt_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "0000000000000000000000000000000000000000000000000000000000000000",
    };
    const auto corrupt_destination = root / "corrupt.txt";
    graph.contents = "data";
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            corrupt_item,
            item_state(corrupt_item, corrupt_destination),
            corrupt_destination,
            metadata,
            space
        ));
        return fail("download with a mismatched Graph hash was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }
    bool corrupt_temporary_exists = false;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                ".corrupt.txt.onedrive-partial-"
            )) {
            corrupt_temporary_exists = true;
        }
    }
    if (std::filesystem::exists(corrupt_destination) ||
        corrupt_temporary_exists || items.partials.contains("corrupt")) {
        return fail("failed hash verification retained resumable state");
    }
    return EXIT_SUCCESS;
}

int test_integrity_diagnostics() {
    namespace detail = onedrive::sync::detail;
    namespace logging = onedrive::logging;
    constexpr std::string_view data_sha256{
        "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7"
    };
    enum class Scenario {
        valid,
        missing_hash,
        remote_mismatch,
        stream_mismatch,
        resumed_mismatch,
        quick_xor_mismatch,
        disk_recheck_failure,
        info_level,
        info_stream_mismatch,
    };
    for (const auto scenario :
         {Scenario::valid,
          Scenario::missing_hash,
          Scenario::remote_mismatch,
          Scenario::stream_mismatch,
          Scenario::resumed_mismatch,
          Scenario::quick_xor_mismatch,
          Scenario::disk_recheck_failure,
          Scenario::info_level,
          Scenario::info_stream_mismatch}) {
        const bool debug_enabled = scenario != Scenario::info_level &&
                                   scenario != Scenario::info_stream_mismatch;
        std::vector<std::pair<logging::Severity, std::string>> messages;
        std::filesystem::path root;
        logging::Session session{{
            .level = debug_enabled ? "debug" : "info",
            .message_sink = [&](logging::Severity level,
                                std::string_view text) {
                messages.emplace_back(level, text);
                if (scenario == Scenario::disk_recheck_failure &&
                    text.starts_with("Download integrity diagnostics")) {
                    for (const auto& entry :
                         std::filesystem::directory_iterator{root}) {
                        if (entry.path().filename().string().starts_with(
                                ".diagnostic.txt.onedrive-partial-"
                            )) {
                            std::filesystem::remove(entry.path());
                        }
                    }
                }
            },
        }};
        DownloadFixture fixture;
        root = fixture.root;
        auto item = remote_item("diagnostic", "diagnostic.txt");
        const bool valid =
            scenario == Scenario::valid || scenario == Scenario::missing_hash;
        if (scenario != Scenario::missing_hash) {
            item.content_hash = onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value = scenario == Scenario::valid ||
                                 scenario == Scenario::stream_mismatch ||
                                 scenario == Scenario::info_stream_mismatch
                             ? std::string{data_sha256}
                             : std::string(64, '0'),
            };
        }
        if (scenario == Scenario::quick_xor_mismatch) {
            item.content_hash = onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
                .value = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            };
        }
        if (scenario == Scenario::stream_mismatch ||
            scenario == Scenario::info_stream_mismatch) {
            fixture.graph.streamed_contents = "bad!";
        }
        const auto destination = root / item.remote_path;
        if (scenario == Scenario::resumed_mismatch) {
            const auto partial =
                root / ".diagnostic.txt.onedrive-partial-previous";
            onedrive::test::write_file(partial, "da");
            fixture.items.save_partial_download({
                .item = item_state(item, destination),
                .temporary_path = partial,
                .completed_bytes = 2,
            });
        }
        bool rejected = false;
        try {
            static_cast<void>(detail::download_atomically(
                fixture.graph,
                fixture.items,
                item,
                item_state(item, destination),
                destination,
                fixture.metadata,
                fixture.space
            ));
        } catch (const detail::DownloadIntegrityError&) {
            rejected = true;
        }
        if (rejected == valid ||
            std::filesystem::exists(destination) != valid ||
            fixture.items.partials.contains(item.id)) {
            return fail("integrity diagnostics changed validation or cleanup");
        }
        const auto contains = [&](std::string_view text) {
            return std::ranges::any_of(messages, [&](const auto& message) {
                return message.second.find(text) != std::string::npos;
            });
        };
        for (const auto& [level, text] : messages) {
            if (text.starts_with("Download integrity diagnostics") ||
                text.starts_with("Download integrity disk recheck")) {
                if (level != logging::Severity::debug) {
                    return fail(
                        "additional integrity diagnostics were not debug"
                    );
                }
            }
        }
        if (contains("Download integrity verification failed") != rejected) {
            return fail("original integrity failure warning was lost");
        }
        if (!debug_enabled) {
            if (contains("Download integrity diagnostics") ||
                contains("Download integrity disk recheck")) {
                return fail(
                    "debug integrity diagnostics bypassed the threshold"
                );
            }
            continue;
        }
        if (!contains(
                "remote_id='diagnostic', eTag='etag', expected_size=4, "
                "downloaded_size=4"
            ) ||
            !contains(
                scenario == Scenario::resumed_mismatch
                    ? "initial_offset=2, checkpoint_offset=4, hash_source=disk"
                    : "initial_offset=0, checkpoint_offset=4, "
                      "hash_source=stream"
            ) ||
            !contains(
                scenario == Scenario::missing_hash
                    ? "remote_algorithm=none, remote_hash=''"
                : scenario == Scenario::quick_xor_mismatch
                    ? "remote_algorithm=QuickXorHash, "
                      "remote_hash='AAAAAAAAAAAAAAAAAAAAAAAAAAA='"
                    : "remote_algorithm=SHA-256"
            ) ||
            !contains("sha256='") || !contains("quick_xor='")) {
            return fail("integrity diagnostics omitted download context");
        }
        const bool rechecked =
            rejected && scenario != Scenario::resumed_mismatch;
        if (contains("Download integrity disk recheck") != rechecked) {
            return fail(
                "integrity disk recheck ran for an unexpected download"
            );
        }
        if (scenario == Scenario::disk_recheck_failure) {
            if (!contains("Download integrity disk recheck failed")) {
                return fail("disk recheck failure was not reported");
            }
        } else if (rechecked) {
            if (!contains(std::string{"sha256='"} + std::string{data_sha256}) ||
                !contains(
                    scenario == Scenario::stream_mismatch
                        ? "sha256_matches_stream=false, "
                          "quick_xor_matches_stream=false"
                        : "sha256_matches_stream=true, "
                          "quick_xor_matches_stream=true"
                )) {
                return fail(
                    "disk recheck did not distinguish streamed hash errors"
                );
            }
        }
        if (rejected) {
            for (const auto& entry :
                 std::filesystem::directory_iterator{root}) {
                if (entry.path().filename().string().starts_with(
                        ".diagnostic.txt.onedrive-partial-"
                    )) {
                    return fail(
                        "integrity diagnostics retained a rejected file"
                    );
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_graph_supported_integrity() {
    namespace graph_detail = onedrive::graph::client_detail;
    std::vector<std::string> messages;
    onedrive::logging::Session session{{
        .level = "debug",
        .message_sink = [&](
                            onedrive::logging::Severity, std::string_view text
                        ) { messages.emplace_back(text); },
    }};
    DownloadFixture fixture;
    graph_detail::Json metadata{
        {"id", "notebook"},
        {"name", "notebook.one"},
        {"eTag", "etag"},
        {"size", 4},
        {"fileSystemInfo", {{"lastModifiedDateTime", "2026-10-02T00:00:00Z"}}},
        {"file",
         {{"hashes",
           {
               {"sha256Hash", std::string(64, '0')},
               {"quickXorHash", "ZAgDHcIAAAAAAAAABAAAAAAAAAA="},
           }}}},
    };
    for (const auto algorithm :
         {onedrive::util::FileHashAlgorithm::quick_xor,
          onedrive::util::FileHashAlgorithm::sha1}) {
        if (algorithm == onedrive::util::FileHashAlgorithm::sha1) {
            metadata["file"]["hashes"].erase("quickXorHash");
            metadata["file"]["hashes"]["sha1Hash"] =
                "A17C9AAA61E80A1BF71D0D850AF4E5BAA9800BBD";
        }
        for (const bool resumed : {false, true}) {
            for (const bool corrupted : {false, true}) {
                messages.clear();
                auto item = graph_detail::parse_drive_item(
                    metadata,
                    "drive item",
                    graph_detail::ContentValidation::strict,
                    graph_detail::DriveItemKind::file_only
                );
                if (!item.content_hash ||
                    item.content_hash->algorithm != algorithm) {
                    return fail(
                        "Graph download selected unsupported SHA-256 metadata"
                    );
                }
                item.id =
                    std::string{
                        onedrive::util::file_hash_algorithm_name(algorithm)
                    } +
                    (resumed ? "-resumed" : "-fresh") +
                    (corrupted ? "-corrupted" : "-valid");
                item.remote_path = item.id + ".one";
                const auto destination = fixture.root / item.remote_path;
                fixture.graph.contents = corrupted ? "bad!" : "data";
                if (resumed) {
                    const auto partial =
                        fixture.root /
                        ("." + item.remote_path + ".onedrive-partial-previous");
                    onedrive::test::write_file(
                        partial, corrupted ? "ba" : "da"
                    );
                    fixture.items.save_partial_download({
                        .item = item_state(item, destination),
                        .temporary_path = partial,
                        .completed_bytes = 2,
                    });
                }
                bool rejected = false;
                try {
                    static_cast<void>(
                        onedrive::sync::detail::download_atomically(
                            fixture.graph,
                            fixture.items,
                            item,
                            item_state(item, destination),
                            destination,
                            fixture.metadata,
                            fixture.space
                        )
                    );
                } catch (
                    const onedrive::sync::detail::DownloadIntegrityError&
                ) {
                    rejected = true;
                }
                if (rejected != corrupted ||
                    fixture.graph.last_initial_offset != (resumed ? 2U : 0U) ||
                    std::filesystem::exists(destination) == corrupted ||
                    fixture.items.partials.contains(item.id)) {
                    return fail(
                        "supported Graph hash failed to verify fresh or "
                        "resumed contents"
                    );
                }
                if (algorithm == onedrive::util::FileHashAlgorithm::sha1 &&
                    !std::ranges::any_of(messages, [&](const auto& message) {
                        return message.starts_with(
                                   "Download integrity diagnostics"
                               ) &&
                               message.contains("hash_source=disk") &&
                               message.contains("remote_algorithm=SHA-1") &&
                               (corrupted ||
                                message.contains(
                                    "sha1='"
                                    "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd'"
                                ));
                    })) {
                    return fail(
                        "SHA-1 download diagnostics omitted the selected disk "
                        "hash"
                    );
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_resume() != EXIT_SUCCESS ||
        test_graph_supported_integrity() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_integrity_diagnostics();
}
