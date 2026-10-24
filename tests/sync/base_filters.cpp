#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/ui/cli/backend.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "base_cases.hpp"

namespace onedrive::test::sync::base_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_selective_sync_refreshes_delta_state() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    const auto sync_list = temporary.path() / "sync_list";
    {
        std::ofstream output{sync_list};
        output << "/Documents/\n";
    }

    FakeGraphClient graph;
    graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
        file("root-file", "root-file.txt", 4),
        file("included", "Documents/included.txt", 4),
        file("excluded", "Pictures/excluded.txt", 7),
    };
    graph.contents["included"] = "data";
    graph.contents["root-file"] = "root";
    graph.contents["excluded"] = "ignored";
    FakeItemStore items;
    items.saved_delta_link = "https://graph.example.test/old-delta";
    items.saved_sync_filter_fingerprint = "old-filter";
    std::filesystem::create_directories(root / "Pictures");
    {
        std::ofstream output{root / "Pictures" / "excluded.txt"};
        output << "ignored";
    }
    items.items.emplace(
        "excluded", tracked_item(root, "excluded", "Pictures/excluded.txt")
    );
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_list = sync_list;

    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != 1 || items.applied_delta.upserts.size() != 2 ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace ||
        items.applied_delta.sync_filter_fingerprint.empty() ||
        !std::filesystem::exists(root / "Pictures/excluded.txt")) {
        return fail(
            "changed selective sync rules did not force a filtered full delta"
        );
    }

    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;
    graph.delta_requests.clear();
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests != std::vector<std::optional<std::string>>{
                                    "https://graph.example.test/delta"
                                }) {
        return fail(
            "unchanged selective sync rules did not reuse the delta cursor"
        );
    }

    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;
    graph.delta_requests.clear();
    config.sync_root_files = true;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != 2 ||
        !std::filesystem::is_regular_file(root / "root-file.txt") ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace) {
        return fail(
            "enabling root files did not force and apply a filtered full delta"
        );
    }
    return EXIT_SUCCESS;
}

int test_filter_policies_apply_in_both_directions() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "filter-policies";
    std::filesystem::create_directories(root / "Ignored");
    {
        std::ofstream marker{root / "Ignored" / ".nosync"};
    }
    {
        std::ofstream output{root / "Ignored" / "local.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / ".local-hidden"};
        output << "data";
    }
    {
        std::ofstream output{root / "local-large.bin"};
        output << "large";
    }
    {
        std::ofstream output{root / "local-small.bin"};
        output << "data";
    }

    FakeGraphClient graph;
    graph.changes = {
        file("remote-small", "remote-small.bin", 4),
        file("remote-large", "remote-large.bin", 5),
        file("remote-hidden", ".remote-hidden", 4),
        file("remote-ignored", "Ignored/remote.txt", 4),
    };
    graph.contents["remote-small"] = "data";
    graph.contents["remote-large"] = "large";
    graph.contents["remote-hidden"] = "data";
    graph.contents["remote-ignored"] = "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    config.dotfiles = onedrive::config::DotfilePolicy::exclude;
    config.maximum_file_size_bytes = 4;

    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != 1 ||
        graph.uploaded_paths != std::vector<std::string>{"local-small.bin"} ||
        !std::filesystem::is_regular_file(root / "remote-small.bin") ||
        std::filesystem::exists(root / "remote-large.bin") ||
        std::filesystem::exists(root / ".remote-hidden") ||
        std::filesystem::exists(root / "Ignored" / "remote.txt") ||
        !std::filesystem::is_regular_file(root / "Ignored" / ".nosync") ||
        items.applied_delta.sync_filter_fingerprint.empty()) {
        return fail("synchronization filter policies were not bidirectional");
    }

    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;
    graph.delta_requests.clear();
    graph.uploaded_paths.clear();
    const auto downloads_before = graph.download_count.load();
    std::filesystem::remove(root / "Ignored" / ".nosync");
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        graph.download_count != downloads_before + 1 ||
        graph.uploaded_paths != std::vector<std::string>{"Ignored/local.txt"} ||
        !std::filesystem::is_regular_file(root / "Ignored" / "remote.txt")) {
        return fail(".nosync removal did not safely re-include its subtree");
    }
    return EXIT_SUCCESS;
}

int test_selective_sync_remote_moves() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto sync_list = temporary.path() / "sync_list";
    {
        std::ofstream output{sync_list};
        output << "/Documents/\n";
    }
    const auto root = temporary.path() / "file-move";
    std::filesystem::create_directories(root / "Documents");
    {
        std::ofstream output{root / "Documents" / "A.txt"};
        output << "data";
    }
    FakeItemStore items;
    items.items.emplace(
        "selective-move",
        tracked_item(root, "selective-move", "Documents/A.txt")
    );
    FakeGraphClient graph;
    graph.changes = {
        file("selective-move", "Archive/A.txt", 4),
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_list = sync_list;
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(root / "Documents" / "A.txt") ||
        std::filesystem::exists(root / "Archive" / "A.txt") ||
        graph.upload_count != 0 ||
        items.upload_suppressions_by_path.size() != 1 ||
        items.items.contains("selective-move")) {
        return fail(
            "move from included to excluded path was not retained safely"
        );
    }
    items.saved_delta_link = items.applied_delta.delta_link;
    items.saved_sync_filter_fingerprint =
        items.applied_delta.sync_filter_fingerprint;

    {
        std::ofstream output{root / "Documents" / "A.txt", std::ios::trunc};
        output << "user";
    }
    graph.changes.clear();
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.upload_count != 0 ||
        items.upload_suppressions_by_path.size() != 1) {
        return fail(
            "modified selectively retained file was uploaded unexpectedly"
        );
    }

    graph.changes = {
        file("selective-move", "Documents/B.txt", 4),
    };
    graph.contents["selective-move"] = "data";
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.download_count != 1 || graph.upload_count != 0 ||
        !std::filesystem::exists(root / "Documents" / "A.txt") ||
        !std::filesystem::exists(root / "Documents" / "B.txt") ||
        items.upload_suppressions_by_path.size() != 1) {
        return fail(
            "move from excluded to included path lost retained protection"
        );
    }

    std::filesystem::rename(
        root / "Documents" / "A.txt", root / "retained-A.txt"
    );
    {
        std::ofstream output{root / "Documents" / "A.txt"};
        output << "new!";
    }
    graph.changes.clear();
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.upload_count != 1 ||
        graph.uploaded_paths != std::vector<std::string>{"Documents/A.txt"} ||
        !items.upload_suppressions_by_path.empty()) {
        return fail(
            "replacement of selectively retained file stayed suppressed"
        );
    }

    const auto directory_root = temporary.path() / "directory-move";
    std::filesystem::create_directories(
        directory_root / "Documents" / "Project"
    );
    {
        std::ofstream output{
            directory_root / "Documents" / "Project" / "one.txt"
        };
        output << "data";
    }
    {
        std::ofstream output{
            directory_root / "Documents" / "Project" / "two.txt"
        };
        output << "data";
    }
    FakeItemStore directory_items;
    directory_items.items.emplace(
        "selective-directory",
        tracked_item(
            directory_root, "selective-directory", "Documents/Project", true
        )
    );
    directory_items.items.emplace(
        "selective-child-one",
        tracked_item(
            directory_root, "selective-child-one", "Documents/Project/one.txt"
        )
    );
    directory_items.items.emplace(
        "selective-child-two",
        tracked_item(
            directory_root, "selective-child-two", "Documents/Project/two.txt"
        )
    );
    FakeGraphClient directory_graph;
    directory_graph.changes = {
        {
            .id = "selective-directory",
            .name = "Project",
            .etag = "directory-etag-2",
            .parent_id = "archive",
            .remote_path = "Archive/Project",
            .directory = true,
        },
        file("selective-child-one", "Archive/Project/one.txt", 4),
        file("selective-child-two", "Archive/Project/two.txt", 4),
    };
    FakeMetrics directory_metrics;
    auto directory_config = config_for(directory_root, false);
    directory_config.sync_list = sync_list;
    directory_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            directory_config,
            directory_graph,
            directory_items,
            directory_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        directory_graph.upload_count != 0 ||
        directory_items.upload_suppressions_by_path.size() != 2 ||
        !directory_items.items.empty() ||
        !std::filesystem::exists(
            directory_root / "Documents" / "Project" / "one.txt"
        ) ||
        !std::filesystem::exists(
            directory_root / "Documents" / "Project" / "two.txt"
        )) {
        return fail(
            "directory move outside selective sync did not retain descendants"
        );
    }

    const auto dry_root = temporary.path() / "dry-move";
    std::filesystem::create_directories(dry_root / "Documents");
    {
        std::ofstream output{dry_root / "Documents" / "A.txt"};
        output << "data";
    }
    FakeItemStore dry_items;
    dry_items.items.emplace(
        "dry-selective-move",
        tracked_item(dry_root, "dry-selective-move", "Documents/A.txt")
    );
    FakeGraphClient dry_graph;
    dry_graph.changes = {
        file("dry-selective-move", "Archive/A.txt", 4),
    };
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.sync_list = sync_list;
    dry_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            dry_config, dry_graph, dry_items, dry_metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        dry_items.apply_count != 0 ||
        !dry_items.upload_suppressions_by_path.empty() ||
        dry_graph.upload_count != 0 ||
        !std::filesystem::exists(dry_root / "Documents" / "A.txt")) {
        return fail("selective move dry run changed local or durable state");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::base_cases
