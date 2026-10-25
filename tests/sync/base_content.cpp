#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/ui/cli/backend.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "base_cases.hpp"

namespace onedrive::test::sync::base_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_content_tag_strategy() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto reused_root = temporary.path() / "ctag-reused";
    std::filesystem::create_directories(reused_root);
    {
        std::ofstream output{reused_root / "file.txt"};
        output << "data";
    }

    auto previous = tracked_item(reused_root, "file", "file.txt");
    previous.etag = "old-etag";
    previous.ctag = "content-version";
    auto metadata_change = file("file", "file.txt", 4);
    metadata_change.etag = "new-etag";
    metadata_change.ctag = "content-version";
    metadata_change.last_modified = "2026-10-05T02:00:00Z";

    FakeGraphClient reused_graph;
    reused_graph.changes = {metadata_change};
    FakeItemStore reused_items;
    reused_items.saved_delta_link = "saved";
    reused_items.items.emplace("file", previous);
    FakeMetrics reused_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(reused_root, false),
            reused_graph,
            reused_items,
            reused_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        reused_graph.download_count != 0 ||
        reused_items.applied_delta.upserts.size() != 1 ||
        reused_items.applied_delta.upserts[0].etag != "new-etag" ||
        reused_items.applied_delta.upserts[0].ctag != "content-version") {
        return fail("unchanged cTag did not reuse local file content");
    }

    // Verify that SharePoint/Business server-enriched files (where remote size
    // and remote hash differ from the local upload baseline) preserve their
    // local size and local content hash when reused via unchanged cTag.
    const auto enriched_root = temporary.path() / "ctag-enriched-reused";
    std::filesystem::create_directories(enriched_root);
    {
        std::ofstream output{enriched_root / "report.docx"};
        output << "data";
    }
    auto enriched_previous =
        tracked_item(enriched_root, "enriched-doc", "report.docx");
    enriched_previous.etag = "old-etag";
    enriched_previous.ctag = "content-version";
    enriched_previous.size = 512;
    enriched_previous.local_size = 4;
    const auto local_hash = enriched_previous.content_hash;
    auto enriched_change = file("enriched-doc", "report.docx", 512);
    enriched_change.etag = "new-etag";
    enriched_change.ctag = "content-version";
    enriched_change.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
        .value = "enriched-server-hash",
    };
    FakeGraphClient enriched_graph;
    enriched_graph.changes = {enriched_change};
    FakeItemStore enriched_items;
    enriched_items.saved_delta_link = "saved";
    enriched_items.items.emplace("enriched-doc", enriched_previous);
    FakeMetrics enriched_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(enriched_root, false),
            enriched_graph,
            enriched_items,
            enriched_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        enriched_graph.download_count != 0 ||
        enriched_items.applied_delta.upserts.size() != 1 ||
        enriched_items.applied_delta.upserts[0].size != 512 ||
        enriched_items.applied_delta.upserts[0].local_size != 4 ||
        enriched_items.applied_delta.upserts[0].content_hash != local_hash) {
        return fail(
            "enriched SharePoint file reuse overwrote local baseline size or "
            "hash"
        );
    }

    const auto changed_root = temporary.path() / "ctag-changed";
    std::filesystem::create_directories(changed_root);
    {
        std::ofstream output{changed_root / "file.txt"};
        output << "data";
    }
    auto changed_previous = tracked_item(changed_root, "file", "file.txt");
    changed_previous.etag = "old-etag";
    changed_previous.ctag = "old-content";
    auto content_change = file("file", "file.txt", 4);
    content_change.etag = "new-etag";
    content_change.ctag = "new-content";

    FakeGraphClient changed_graph;
    changed_graph.changes = {content_change};
    changed_graph.contents["file"] = "next";
    FakeItemStore changed_items;
    changed_items.saved_delta_link = "saved";
    changed_items.items.emplace("file", changed_previous);
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        changed_graph.download_count != 1) {
        return fail("changed cTag did not download remote content");
    }

    const auto missing_root = temporary.path() / "ctag-missing";
    std::filesystem::create_directories(missing_root);
    {
        std::ofstream output{missing_root / "file.txt"};
        output << "data";
    }
    auto missing_previous = tracked_item(missing_root, "file", "file.txt");
    missing_previous.etag = "old-etag";
    missing_previous.ctag = "old-content";
    auto missing_ctag = file("file", "file.txt", 4);
    missing_ctag.etag = "new-etag";

    FakeGraphClient missing_graph;
    missing_graph.changes = {missing_ctag};
    missing_graph.contents["file"] = "next";
    FakeItemStore missing_items;
    missing_items.saved_delta_link = "saved";
    missing_items.items.emplace("file", missing_previous);
    FakeMetrics missing_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(missing_root, false),
            missing_graph,
            missing_items,
            missing_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        missing_graph.download_count != 1) {
        return fail("missing cTag bypassed the eTag fallback");
    }
    return EXIT_SUCCESS;
}

int test_malware_file_is_blocked_without_overwriting_local_data() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto destination = root / "reported-malware.exe";
    {
        std::ofstream output{destination, std::ios::binary};
        output << "local data";
    }

    FakeGraphClient graph;
    auto malware = file("malware", "reported-malware.exe", 4);
    malware.malware = true;
    graph.changes = {malware};
    graph.contents["malware"] = "evil";
    FakeItemStore items;
    FakeMetrics metrics;
    if (onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::blocked ||
        graph.download_count != 0 || items.applied_delta.upserts.size() != 0 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].reason_code !=
            "malware_detected" ||
        !metrics.last_success) {
        return fail("Graph malware item was not isolated from downloads");
    }
    std::ifstream input{destination, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}
    };
    if (contents != "local data") {
        return fail("Graph malware item overwrote an existing local file");
    }
    return EXIT_SUCCESS;
}

int test_blocked_items_continue_and_retry() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("invalid", std::string(256, 'x'), 4),
        file("good", "good.txt", 4),
    };
    graph.contents["good"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    if (onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::blocked ||
        graph.download_count != 1 ||
        !std::filesystem::exists(root / "good.txt") || items.apply_count != 1 ||
        items.applied_delta.blocked_upserts.size() != 1 ||
        items.applied_delta.blocked_upserts[0].remote_id != "invalid" ||
        items.applied_delta.upserts.size() != 1 || !metrics.last_success) {
        return fail("invalid filename prevented an independent download");
    }

    const auto parent_root = temporary.path() / "parent-conflict";
    std::filesystem::create_directories(parent_root);
    {
        std::ofstream output{parent_root / "Documents"};
        output << "local file";
    }
    FakeGraphClient parent_graph;
    parent_graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
        file("child", "Documents/child.txt", 4),
        file("independent", "independent.txt", 4),
    };
    parent_graph.contents["independent"] = "data";
    FakeItemStore parent_items;
    FakeMetrics parent_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(parent_root, false),
            parent_graph,
            parent_items,
            parent_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        parent_graph.download_count != 1 ||
        !std::filesystem::exists(parent_root / "independent.txt") ||
        parent_items.applied_delta.blocked_upserts.size() != 2 ||
        parent_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict" ||
        parent_items.applied_delta.blocked_upserts[1].reason_code !=
            "blocked_by_parent") {
        return fail("blocked directory did not isolate its descendants");
    }

    const auto retry_root = temporary.path() / "retry";
    FakeGraphClient retry_graph;
    retry_graph.contents["retry"] = "data";
    FakeItemStore retry_items;
    retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "retry",
            .parent_id = "root",
            .name = "retry.txt",
            .etag = "retry-etag",
            .remote_path = "retry.txt",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "local_modification",
            .reason_message = "previous conflict",
            .attempt_count = 1,
        },
    };
    FakeMetrics retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(retry_root, false),
            retry_graph,
            retry_items,
            retry_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        retry_graph.download_count != 1 ||
        !std::filesystem::exists(retry_root / "retry.txt") ||
        retry_items.applied_delta.blocked_upserts.size() != 0 ||
        retry_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"retry"} ||
        !retry_metrics.last_success) {
        return fail("persisted blocked item was not retried and cleared");
    }

    const auto hash_retry_root = temporary.path() / "hash-retry";
    FakeGraphClient hash_retry_graph;
    hash_retry_graph.contents["hash-retry"] = "data";
    FakeItemStore hash_retry_items;
    hash_retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    hash_retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "hash-retry",
            .parent_id = "root",
            .name = "hash-retry.txt",
            .etag = "retry-etag",
            .remote_path = "hash-retry.txt",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "local_modification",
            .reason_message = "previous conflict",
            .attempt_count = 1,
            .content_hash = onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value = "000000000000000000000000000000000000000000000000"
                         "0000000000000000",
            },
        },
    };
    FakeMetrics hash_retry_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(hash_retry_root, false),
            hash_retry_graph,
            hash_retry_items,
            hash_retry_metrics,
            &default_console
        }
                              .synchronize());
        return fail("blocked retry ignored its persisted content hash");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(hash_retry_root / "hash-retry.txt") ||
        !hash_retry_items.partials.empty() || hash_retry_metrics.last_success) {
        return fail("failed blocked hash retry retained resumable state");
    }

    const auto malware_retry_root = temporary.path() / "malware-retry";
    FakeGraphClient malware_retry_graph;
    malware_retry_graph.contents["malware-retry"] = "evil";
    FakeItemStore malware_retry_items;
    malware_retry_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    malware_retry_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "malware-retry",
            .parent_id = "root",
            .name = "malware-retry.exe",
            .etag = "retry-etag",
            .remote_path = "malware-retry.exe",
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 4,
            .reason_code = "malware_detected",
            .reason_message =
                "Microsoft Graph marked the remote file as malware",
            .attempt_count = 1,
        },
    };
    FakeMetrics malware_retry_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(malware_retry_root, false),
            malware_retry_graph,
            malware_retry_items,
            malware_retry_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        malware_retry_graph.download_count != 0 ||
        malware_retry_items.applied_delta.blocked_upserts.size() != 1 ||
        malware_retry_items.applied_delta.blocked_upserts[0].reason_code !=
            "malware_detected" ||
        !malware_retry_metrics.last_success) {
        return fail("persisted malware item was retried as a download");
    }

    const auto cleared_root = temporary.path() / "malware-cleared";
    FakeGraphClient cleared_graph;
    cleared_graph.changes = {
        file("malware-cleared", "malware-cleared.txt", 4),
    };
    cleared_graph.contents["malware-cleared"] = "data";
    FakeItemStore cleared_items;
    cleared_items.saved_delta_link =
        "https://graph.example.test/delta?token=saved";
    cleared_items.blocked = {
        {
            .drive_id = "me",
            .remote_id = "malware-cleared",
            .parent_id = "root",
            .name = "malware-cleared.txt",
            .etag = "old-malware-etag",
            .remote_path = "malware-cleared.txt",
            .last_modified = "2026-10-01T00:00:00Z",
            .size = 4,
            .reason_code = "malware_detected",
            .reason_message =
                "Microsoft Graph marked the remote file as malware",
            .attempt_count = 1,
        },
    };
    FakeMetrics cleared_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cleared_root, false),
            cleared_graph,
            cleared_items,
            cleared_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        cleared_graph.download_count != 1 ||
        !std::filesystem::exists(cleared_root / "malware-cleared.txt") ||
        !cleared_items.applied_delta.blocked_upserts.empty() ||
        cleared_items.applied_delta.blocked_removals !=
            std::vector<std::string>{"malware-cleared"} ||
        !cleared_metrics.last_success) {
        return fail("cleared Graph malware marker did not resume downloading");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::base_cases
