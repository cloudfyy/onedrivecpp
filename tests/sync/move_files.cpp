#include "support.hpp"
#include "move_cases.hpp"
#include "onedrive/exit_code.hpp"
#include <algorithm>

namespace onedrive::test::sync::move_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_move_renames() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto renamed_root = temporary.path() / "renamed";
    std::filesystem::create_directories(renamed_root);
    {
        std::ofstream output{renamed_root / "old.txt"};
        output << "data";
    }
    FakeGraphClient renamed_graph;
    renamed_graph.changes = {file("renamed", "new.txt", 4)};
    FakeItemStore renamed_items;
    renamed_items.saved_delta_link = "saved";
    renamed_items.items.emplace(
        "renamed", tracked_item(renamed_root, "renamed", "old.txt")
    );
    FakeMetrics renamed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(renamed_root, false),
            renamed_graph,
            renamed_items,
            renamed_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(renamed_root / "old.txt") ||
        !std::filesystem::exists(renamed_root / "new.txt") ||
        renamed_graph.download_count != 0 ||
        renamed_items.applied_delta.upserts.size() != 1 ||
        renamed_items.applied_delta.upserts[0].remote_path != "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_path !=
            renamed_root / "new.txt" ||
        renamed_items.applied_delta.upserts[0].local_size != 4 ||
        !renamed_items.pending_moves_by_id.empty() ||
        !renamed_metrics.last_success) {
        return fail("remote file rename was not applied locally");
    }

    const auto sha1_root = temporary.path() / "sha1-rename";
    std::filesystem::create_directory(sha1_root);
    onedrive::test::write_file(sha1_root / "old.txt", "data");
    auto sha1_item = file("sha1-move", "new.txt", 4);
    sha1_item.etag = "new-etag";
    sha1_item.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha1,
        .value = "A17C9AAA61E80A1BF71D0D850AF4E5BAA9800BBD",
    };
    FakeGraphClient sha1_graph;
    sha1_graph.changes = {sha1_item};
    FakeItemStore sha1_items;
    sha1_items.saved_delta_link = "saved";
    sha1_items.items.emplace(
        "sha1-move", tracked_item(sha1_root, "sha1-move", "old.txt")
    );
    FakeMetrics sha1_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(sha1_root, false),
            sha1_graph,
            sha1_items,
            sha1_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(sha1_root / "old.txt") ||
        onedrive::test::read_file(sha1_root / "new.txt") != "data" ||
        sha1_graph.download_count != 0) {
        return fail("SHA-1-verified remote move downloaded unchanged content");
    }

    const auto enriched_root = temporary.path() / "enriched-rename";
    std::filesystem::create_directory(enriched_root);
    onedrive::test::write_file(enriched_root / "report.docx", "data");
    auto enriched_previous =
        tracked_item(enriched_root, "sp-enriched-move", "report.docx");
    enriched_previous.size = 128;
    enriched_previous.local_size = 4;
    enriched_previous.etag = "etag-enriched-1";
    enriched_previous.ctag = "ctag-enriched-1";
    auto enriched_moved = file("sp-enriched-move", "archive/report.docx", 128);
    enriched_moved.etag = "etag-enriched-2";
    enriched_moved.ctag = "ctag-enriched-1";
    enriched_moved.last_modified = enriched_previous.last_modified;
    enriched_moved.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
        .value = "serverEnrichedQuickXorHash==",
    };
    FakeGraphClient enriched_graph;
    enriched_graph.changes = {enriched_moved};
    FakeItemStore enriched_items;
    enriched_items.saved_delta_link = "saved";
    enriched_items.items.emplace("sp-enriched-move", enriched_previous);
    FakeMetrics enriched_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(enriched_root, false),
            enriched_graph,
            enriched_items,
            enriched_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(enriched_root / "report.docx") ||
        onedrive::test::read_file(enriched_root / "archive/report.docx") !=
            "data" ||
        enriched_graph.download_count != 0 ||
        enriched_items.applied_delta.upserts.size() != 1 ||
        enriched_items.applied_delta.upserts[0].local_size != 4 ||
        enriched_items.applied_delta.upserts[0].content_hash !=
            enriched_previous.content_hash) {
        return fail(
            "server-enriched SharePoint/Business remote move did not preserve "
            "local baseline"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_move_adoption() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto adopted_root = temporary.path() / "adopted";
    std::filesystem::create_directories(adopted_root);
    {
        std::ofstream output{adopted_root / "new.txt"};
        output << "data";
    }
    FakeGraphClient adopted_graph;
    adopted_graph.changes = {file("adopted", "new.txt", 4)};
    FakeItemStore adopted_items;
    adopted_items.saved_delta_link = "saved";
    auto adopted_state = tracked_item(adopted_root, "adopted", "new.txt");
    adopted_state.name = "old.txt";
    adopted_state.remote_path = "old.txt";
    adopted_state.local_path = adopted_root / "old.txt";
    adopted_items.items.emplace("adopted", std::move(adopted_state));
    FakeMetrics adopted_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(adopted_root, false),
            adopted_graph,
            adopted_items,
            adopted_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        adopted_graph.download_count != 0 ||
        adopted_items.applied_delta.upserts.size() != 1 ||
        adopted_items.applied_delta.upserts[0].local_path !=
            adopted_root / "new.txt") {
        return fail("interrupted remote move destination was not adopted");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_changed_content() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto changed_root = temporary.path() / "changed";
    std::filesystem::create_directories(changed_root);
    {
        std::ofstream output{changed_root / "old.txt"};
        output << "data";
    }
    auto changed = file("changed", "folder/new.txt", 4);
    changed.etag = "changed-etag";
    changed.last_modified = "2026-10-04T10:00:00Z";
    changed.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value =
            "c6c1c9a9c8543f1e4cd980064cf1625eeb61a90703b2464fff039f21682508b3",
    };
    FakeGraphClient changed_graph;
    changed_graph.changes = {changed};
    changed_graph.contents["changed"] = "next";
    FakeItemStore changed_items;
    changed_items.saved_delta_link = "saved";
    changed_items.items.emplace(
        "changed", tracked_item(changed_root, "changed", "old.txt")
    );
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(changed_root / "old.txt") ||
        changed_graph.download_count != 1) {
        return fail("moved file with changed content was not downloaded");
    }
    {
        std::ifstream input{changed_root / "folder" / "new.txt"};
        const std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (content != "next") {
            return fail("moved file did not receive changed remote content");
        }
    }
    return EXIT_SUCCESS;
}

int test_remote_move_preview() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto dry_root = temporary.path() / "dry-move";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "old.txt"};
        output << "data";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    dry_items.items.emplace("dry", tracked_item(dry_root, "dry", "old.txt"));
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("dry", "new.txt", 4)};
    FakeMetrics dry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(dry_root, true),
            dry_graph,
            dry_items,
            dry_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(dry_root / "old.txt") ||
        std::filesystem::exists(dry_root / "new.txt") ||
        dry_items.apply_count != 0) {
        return fail("remote move dry run changed local state");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::move_cases
