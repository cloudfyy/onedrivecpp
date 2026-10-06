#include "support.hpp"

namespace {

using namespace onedrive::test::download;

int test_txn() {
    namespace detail = onedrive::sync::detail;
    DownloadFixture fixture;
    const auto& root = fixture.root;
    const auto& metadata = fixture.metadata;
    const auto& safe_root = fixture.safe_root;
    auto& space = fixture.space;
    auto& graph = fixture.graph;
    auto& items = fixture.items;
    auto installed_item = remote_item("installed", "installed.txt");
    installed_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
    };
    const auto destination = root / "installed.txt";
    const auto installed = detail::download_atomically(
        graph,
        items,
        installed_item,
        item_state(installed_item, destination),
        destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(destination) || !items.pending.empty() ||
        !items.partials.empty() || !items.find("me", "installed") ||
        installed.local_size != 4 || installed.local_modified_ticks == 0 ||
        installed.local_device == 0 || installed.local_inode == 0 ||
        graph.last_expected_etag != installed_item.etag ||
        !has_remote_modified_time(destination)) {
        return fail("atomic download transaction did not commit");
    }

    const auto raced_item = remote_item("raced", "raced.txt");
    const auto raced_destination = root / "raced.txt";
    {
        std::ofstream output{raced_destination, std::ios::binary};
        output << "local";
    }
    onedrive::graph::GraphClient graph_proxy{
        onedrive::util::borrowed_proxy, graph
    };
    onedrive::storage::ItemStore store_proxy{
        onedrive::util::borrowed_proxy, items
    };
    bool changed_after_journal = false;
    items.pending_download_saved = [&] {
        if (changed_after_journal) {
            return;
        }
        changed_after_journal = true;
        std::ofstream output{
            raced_destination, std::ios::binary | std::ios::trunc
        };
        output << "changed";
    };
    const auto raced = detail::commit_download(
        store_proxy,
        detail::SafeSyncRoot{root},
        metadata,
        detail::prepare_download(
            graph_proxy,
            store_proxy,
            raced_item,
            item_state(raced_item, raced_destination),
            raced_destination,
            detail::capture_local_file_baseline(raced_destination),
            metadata,
            space,
            {}
        ),
        {
            .local_conflict = onedrive::config::LocalConflictPolicy::backup,
            .preserve_local = true,
        }
    );
    items.pending_download_saved = {};
    std::ifstream raced_input{raced_destination, std::ios::binary};
    const std::string raced_contents{
        std::istreambuf_iterator<char>{raced_input},
        std::istreambuf_iterator<char>{}
    };
    if (!changed_after_journal || raced_contents != "data" ||
        raced.local_size != 4 || items.pending.contains("raced") ||
        items.partials.contains("raced")) {
        return fail("journaled download did not retry through prepared state");
    }

    const auto recover_item = remote_item("recover", "recover.txt");
    const auto recover_destination = root / "recover.txt";
    items.fail_upsert = true;
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            recover_item,
            item_state(recover_item, recover_destination),
            recover_destination,
            metadata,
            space
        ));
        return fail("post-install persistence failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(recover_destination) ||
        items.pending.size() != 1) {
        return fail("failed transaction did not preserve recovery journal");
    }

    items.fail_upsert = false;
    detail::recover_pending_downloads(items, root, "me", metadata);
    if (!items.pending.empty() || !items.find("me", "recover") ||
        !std::filesystem::exists(recover_destination) ||
        !has_remote_modified_time(recover_destination)) {
        return fail("pending installed download was not recovered");
    }

    graph.contents = "short";
    const auto mismatch_item = remote_item("mismatch", "mismatch.txt");
    const auto mismatch_destination = root / "mismatch.txt";
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            mismatch_item,
            item_state(mismatch_item, mismatch_destination),
            mismatch_destination,
            metadata,
            space
        ));
        return fail("download size mismatch was accepted");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(mismatch_destination) ||
        items.pending.contains("mismatch")) {
        return fail("size mismatch left installed or journaled state");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_txn();
}
