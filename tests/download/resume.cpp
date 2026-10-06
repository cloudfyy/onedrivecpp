#include "support.hpp"

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

} // namespace

int main() {
    return test_resume();
}
