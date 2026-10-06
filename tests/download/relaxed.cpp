#include "support.hpp"

namespace {

using namespace onedrive::test::download;

int test_relaxed() {
    namespace detail = onedrive::sync::detail;
    DownloadFixture fixture;
    const auto& root = fixture.root;
    const auto& metadata = fixture.metadata;
    const auto& safe_root = fixture.safe_root;
    auto& space = fixture.space;
    auto& graph = fixture.graph;
    auto& items = fixture.items;

    graph.send_checkpoint = false;
    graph.progress_updates = {0, 2, 2, 4};
    auto relaxed_item = remote_item("relaxed", "protected.heic");
    relaxed_item.size = 2;
    relaxed_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "0000000000000000000000000000000000000000000000000000000000000000",
    };
    relaxed_item.validate_content = false;
    const auto relaxed_destination = root / "protected.heic";
    const auto relaxed = detail::download_atomically(
        graph,
        items,
        relaxed_item,
        item_state(relaxed_item, relaxed_destination),
        relaxed_destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(relaxed_destination) ||
        relaxed.local_size != 4 || relaxed.size != 4) {
        return fail(
            "relaxed download validation did not accept actual file metadata"
        );
    }

    auto zero_size_relaxed_item =
        remote_item("relaxed-zero", "protected-zero.heic");
    zero_size_relaxed_item.size = 0;
    zero_size_relaxed_item.validate_content = false;
    const auto zero_size_relaxed_destination = root / "protected-zero.heic";
    const auto zero_size_relaxed = detail::download_atomically(
        graph,
        items,
        zero_size_relaxed_item,
        item_state(zero_size_relaxed_item, zero_size_relaxed_destination),
        zero_size_relaxed_destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(zero_size_relaxed_destination) ||
        zero_size_relaxed.local_size != 4 || zero_size_relaxed.size != 4) {
        return fail(
            "zero-size relaxed download did not reserve its actual bytes"
        );
    }

    auto overreported_relaxed_item =
        remote_item("relaxed-large", "protected-large.heic");
    overreported_relaxed_item.size = 100;
    overreported_relaxed_item.validate_content = false;
    const auto overreported_relaxed_destination = root / "protected-large.heic";
    detail::DownloadSpaceCoordinator actual_size_space{
        root, 0, [](const std::filesystem::path&) { return std::uintmax_t{4}; }
    };
    const auto overreported_relaxed = detail::download_atomically(
        graph,
        items,
        overreported_relaxed_item,
        item_state(overreported_relaxed_item, overreported_relaxed_destination),
        overreported_relaxed_destination,
        metadata,
        actual_size_space
    );
    if (!std::filesystem::exists(overreported_relaxed_destination) ||
        overreported_relaxed.local_size != 4 ||
        overreported_relaxed.size != 4) {
        return fail("relaxed download reserved an inaccurate remote file size");
    }

    graph.progress_updates.clear();
    auto insufficient_relaxed_item =
        remote_item("relaxed-space", "protected-space.heic");
    insufficient_relaxed_item.size = 2;
    insufficient_relaxed_item.validate_content = false;
    const auto insufficient_relaxed_destination = root / "protected-space.heic";
    detail::DownloadSpaceCoordinator insufficient_relaxed_space{
        root, 0, [](const std::filesystem::path&) { return std::uintmax_t{3}; }
    };
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            insufficient_relaxed_item,
            item_state(
                insufficient_relaxed_item, insufficient_relaxed_destination
            ),
            insufficient_relaxed_destination,
            metadata,
            insufficient_relaxed_space
        ));
        return fail("relaxed download exceeded dynamically available space");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(insufficient_relaxed_destination) ||
        items.partials.contains(insufficient_relaxed_item.id)) {
        return fail("failed relaxed space expansion retained download state");
    }
    auto released_space = insufficient_relaxed_space.acquire(3);
    if (released_space.remaining() != 3) {
        return fail("failed relaxed expansion did not release its reservation");
    }

    const auto relaxed_partial =
        root / ".protected-partial.heic.onedrive-partial-previous";
    {
        std::ofstream output{relaxed_partial, std::ios::binary};
        output << "da";
    }
    auto relaxed_partial_item =
        remote_item("relaxed-partial", "protected-partial.heic");
    relaxed_partial_item.validate_content = false;
    const auto relaxed_partial_destination = root / "protected-partial.heic";
    items.partials.emplace(
        relaxed_partial_item.id,
        onedrive::storage::PartialDownload{
            .item =
                item_state(relaxed_partial_item, relaxed_partial_destination),
            .temporary_path = relaxed_partial,
            .completed_bytes = 2,
        }
    );
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        relaxed_partial_item,
        item_state(relaxed_partial_item, relaxed_partial_destination),
        relaxed_partial_destination,
        metadata,
        space
    ));
    if (graph.last_initial_offset != 0 ||
        std::filesystem::exists(relaxed_partial) ||
        items.partials.contains(relaxed_partial_item.id)) {
        return fail("relaxed download reused a remote-size partial file");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_relaxed();
}
