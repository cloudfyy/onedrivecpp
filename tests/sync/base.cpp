#include "support.hpp"

#include "onedrive/cli/backend.hpp"

namespace {

using namespace onedrive::test::sync;

class ProgressBackend final : public onedrive::cli::ConsoleBackend {
public:
    void emit(const onedrive::cli::ConsoleEvent& event) override {
        if (const auto* progress =
                std::get_if<onedrive::cli::DownloadProgressEvent>(&event)) {
            downloads.push_back(*progress);
        }
    }

    bool confirm(
        const onedrive::cli::ConfirmationRequest&
    ) override {
        return false;
    }

    onedrive::cli::OutputMode output_mode() const noexcept override {
        return onedrive::cli::OutputMode::text;
    }

    onedrive::cli::UiMode ui_mode() const noexcept override {
        return onedrive::cli::UiMode::console;
    }

    std::vector<onedrive::cli::DownloadProgressEvent> downloads;
};

int test_engine_owns_configuration() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "owned-config";
    FakeGraphClient graph;
    FakeItemStore items;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    onedrive::sync::SyncEngine engine{
        config, graph, items, metrics, &default_console
    };
    config.sync_data_mount_point = temporary.path() / "missing-mount";
    config.drive_id = "mutated";

    if (engine.synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{std::nullopt} ||
        items.applied_delta.drive_id != "me") {
        return fail("sync engine did not preserve its owned configuration");
    }
    return EXIT_SUCCESS;
}

int test_dry_run_and_success() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto guarded_root = temporary.path() / "guarded";
    FakeGraphClient guarded_graph;
    FakeItemStore guarded_items;
    FakeMetrics guarded_metrics;
    auto guarded_config = config_for(guarded_root, false);
    guarded_config.sync_data_mount_point = temporary.path();
    std::ostringstream guarded_output;
    std::ostringstream guarded_error;
    const onedrive::cli::Console guarded_console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
        },
        guarded_output,
        guarded_error
    };
    if (onedrive::sync::SyncEngine{
            guarded_config,
            guarded_graph,
            guarded_items,
            guarded_metrics,
            &guarded_console
        }
                .synchronize() == 0 ||
        !guarded_graph.delta_requests.empty() ||
        std::filesystem::exists(guarded_root) ||
        guarded_metrics.last_success ||
        !guarded_output.str().empty() ||
        !guarded_error.str().contains(
            "\"event\":\"sync_mount_unavailable\""
        ) ||
        !guarded_error.str().contains("not currently mounted")) {
        return fail("missing sync mount was not blocked before synchronization");
    }

    const auto dry_root = temporary.path() / "dry";
    FakeGraphClient dry_graph;
    dry_graph.changes = {file("file", "Documents/file.txt", 4)};
    dry_graph.contents["file"] = "data";
    FakeItemStore dry_items;
    FakeMetrics dry_metrics;
    const auto dry_config = config_for(dry_root, true);
    if (onedrive::sync::SyncEngine{
            dry_config, dry_graph, dry_items, dry_metrics, &default_console
        }
                .synchronize() != 0 ||
        dry_graph.download_count != 0 || dry_items.apply_count != 0 ||
        std::filesystem::exists(dry_root)) {
        return fail("dry run modified files or synchronization state");
    }

    const auto root = temporary.path() / "files";
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
        file("file", "Documents/file.txt", 4),
    };
    graph.contents["file"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    std::ostringstream progress_output;
    std::ostringstream progress_error;
    const onedrive::cli::Console console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::text,
        },
        progress_output,
        progress_error
    };
    if (onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
                .synchronize() != 0 ||
        graph.download_count != 1 || items.upsert_count != 1 ||
        items.apply_count != 1 || !metrics.last_success ||
        !progress_output.str().contains("DL: 0/1 files, 50% (2 B/4 B)") ||
        !progress_output.str().contains("Done: 1/1 files, 100% (4 B/4 B)") ||
        !progress_error.str().empty()) {
        return fail("successful download did not commit synchronization state");
    }
    std::ifstream input{root / "Documents/file.txt", std::ios::binary};
    std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}
    };
    if (contents != "data" || items.applied_delta.upserts.size() != 2 ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace ||
        items.applied_delta.upserts[1].local_size != 4 ||
        items.applied_delta.upserts[1].local_modified_ticks == 0) {
        return fail("downloaded file or local snapshot was incorrect");
    }
    return EXIT_SUCCESS;
}

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
                .synchronize() != 0 ||
        reused_graph.download_count != 0 ||
        reused_items.applied_delta.upserts.size() != 1 ||
        reused_items.applied_delta.upserts[0].etag != "new-etag" ||
        reused_items.applied_delta.upserts[0].ctag != "content-version") {
        return fail("unchanged cTag did not reuse local file content");
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
        missing_graph.download_count != 1) {
        return fail("missing cTag bypassed the eTag fallback");
    }
    return EXIT_SUCCESS;
}

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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 0 ||
        dry_items.apply_count != 0 ||
        !dry_items.upload_suppressions_by_path.empty() ||
        dry_graph.upload_count != 0 ||
        !std::filesystem::exists(dry_root / "Documents" / "A.txt")) {
        return fail("selective move dry run changed local or durable state");
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
                .synchronize() != 2 ||
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

int test_failure_and_conflict() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.changes = {
        file("first", "first.txt", 4),
        file("second", "second.txt", 4),
    };
    graph.contents["first"] = "data";
    graph.failing_id = "second";
    FakeItemStore items;
    FakeMetrics metrics;
    const auto config = config_for(root, false);
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("download failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (items.apply_count != 0 || items.upsert_count != 1 ||
        !std::filesystem::exists(root / "first.txt") ||
        std::filesystem::exists(root / "second.txt") || metrics.last_success) {
        return fail("failed download advanced state or left an invalid file");
    }

    const auto independent_root = temporary.path() / "independent";
    FakeGraphClient independent_graph;
    independent_graph.changes = {
        file("failed-first", "failed-first.txt", 4),
        file("completed-second", "completed-second.txt", 4),
    };
    independent_graph.contents["completed-second"] = "data";
    independent_graph.failing_id = "failed-first";
    independent_graph.downloads_started_before_failure = 2;
    FakeItemStore independent_items;
    FakeMetrics independent_metrics;
    auto independent_config = config_for(independent_root, false);
    independent_config.download_concurrency = 2;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            independent_config,
            independent_graph,
            independent_items,
            independent_metrics,
            &default_console
        }
                              .synchronize());
        return fail("concurrent download failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (independent_items.upsert_count != 1 ||
        !std::filesystem::exists(independent_root / "completed-second.txt") ||
        std::filesystem::exists(independent_root / "failed-first.txt") ||
        independent_metrics.last_success) {
        return fail(
            "an independently completed download was discarded after failure"
        );
    }
    independent_graph.failing_id.clear();
    independent_graph.contents["failed-first"] = "data";
    independent_graph.download_count.store(0, std::memory_order_relaxed);
    auto progress_backend = std::make_unique<ProgressBackend>();
    auto* captured_progress = progress_backend.get();
    const onedrive::cli::Console progress_console{
        std::move(progress_backend)
    };
    if (onedrive::sync::SyncEngine{
            independent_config,
            independent_graph,
            independent_items,
            independent_metrics,
            &progress_console
        }
            .synchronize() != 0 ||
        independent_graph.download_count.load(std::memory_order_relaxed) != 1 ||
        captured_progress->downloads.size() < 2 ||
        captured_progress->downloads.front().completed_files != 1 ||
        captured_progress->downloads.front().file_count != 2 ||
        captured_progress->downloads.front().downloaded != 4 ||
        captured_progress->downloads.front().total != 8 ||
        captured_progress->downloads.back().completed_files != 2 ||
        captured_progress->downloads.back().file_count != 2 ||
        !std::filesystem::exists(independent_root / "failed-first.txt") ||
        !std::filesystem::exists(
            independent_root / "completed-second.txt"
        ) ||
        !independent_metrics.last_success) {
        return fail(
            "retry downloaded a file that completed before interruption"
        );
    }
    independent_graph.download_count.store(0, std::memory_order_relaxed);
    auto reused_backend = std::make_unique<ProgressBackend>();
    auto* captured_reused = reused_backend.get();
    const onedrive::cli::Console reused_console{
        std::move(reused_backend)
    };
    if (onedrive::sync::SyncEngine{
            independent_config,
            independent_graph,
            independent_items,
            independent_metrics,
            &reused_console
        }
            .synchronize() != 0 ||
        independent_graph.download_count.load(std::memory_order_relaxed) != 0 ||
        captured_reused->downloads.size() != 1 ||
        captured_reused->downloads.front().completed_files != 2 ||
        captured_reused->downloads.front().file_count != 2 ||
        captured_reused->downloads.front().downloaded != 8 ||
        captured_reused->downloads.front().total != 8 ||
        captured_reused->downloads.front().state !=
            onedrive::util::ProgressState::completed) {
        return fail("fully reused downloads did not report completed progress");
    }

    const auto changed_root = temporary.path() / "changed-during-download";
    FakeGraphClient changed_graph;
    changed_graph.changes = {
        file("changed", "changed.txt", 4),
    };
    changed_graph.contents["changed"] = "data";
    changed_graph.before_download_write =
        [destination = changed_root / "changed.txt"](const std::string&) {
            std::ofstream output{destination};
            output << "user data";
        };
    FakeItemStore changed_items;
    FakeMetrics changed_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(changed_root, false),
            changed_graph,
            changed_items,
            changed_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        changed_items.upsert_count != 0 || changed_items.apply_count != 1 ||
        changed_items.applied_delta.blocked_upserts.size() != 1 ||
        changed_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !changed_items.pending.empty() || !changed_metrics.last_success) {
        return fail("download-time local modification was not isolated");
    }
    {
        std::ifstream input{changed_root / "changed.txt"};
        std::string contents{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (contents != "user data") {
            return fail("download overwrote a file created during transfer");
        }
    }

    const auto conflict_root = temporary.path() / "conflict";
    std::filesystem::create_directories(conflict_root);
    {
        std::ofstream output{conflict_root / "existing.txt"};
        output << "user data";
    }
    FakeGraphClient conflict_graph;
    conflict_graph.changes = {file("existing", "existing.txt", 4)};
    conflict_graph.contents["existing"] = "data";
    FakeItemStore conflict_items;
    FakeMetrics conflict_metrics;
    const auto conflict_config = config_for(conflict_root, false);
    if (onedrive::sync::SyncEngine{
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        conflict_graph.download_count != 0 || conflict_items.apply_count != 1 ||
        conflict_items.applied_delta.blocked_upserts.size() != 1 ||
        conflict_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !conflict_metrics.last_success) {
        return fail("local conflict was not isolated and persisted");
    }

    const auto backup_root = temporary.path() / "backup-conflict";
    std::filesystem::create_directories(backup_root);
    {
        std::ofstream output{backup_root / "existing.txt"};
        output << "user data";
    }
    FakeGraphClient backup_graph;
    backup_graph.changes = {file("existing", "existing.txt", 4)};
    backup_graph.contents["existing"] = "data";
    FakeItemStore backup_items;
    FakeMetrics backup_metrics;
    auto backup_config = config_for(backup_root, false);
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            backup_config,
            backup_graph,
            backup_items,
            backup_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        backup_graph.download_count != 1 || backup_items.upsert_count != 1 ||
        !backup_items.applied_delta.blocked_upserts.empty() ||
        !backup_metrics.last_success) {
        return fail("safeBackup mode did not resolve a local conflict");
    }
    std::filesystem::path preserved;
    for (const auto& entry : std::filesystem::directory_iterator{backup_root}) {
        if (entry.path().filename().string().starts_with(
                "existing.safeBackup-"
            )) {
            preserved = entry.path();
        }
    }
    std::ifstream installed_input{backup_root / "existing.txt"};
    std::ifstream preserved_input{preserved};
    const std::string installed{
        std::istreambuf_iterator<char>{installed_input},
        std::istreambuf_iterator<char>{}
    };
    const std::string preserved_contents{
        std::istreambuf_iterator<char>{preserved_input},
        std::istreambuf_iterator<char>{}
    };
    if (preserved.empty() || installed != "data" ||
        preserved_contents != "user data") {
        return fail("safeBackup mode did not preserve conflicting content");
    }

    const auto late_backup_root =
        temporary.path() / "backup-created-during-download";
    FakeGraphClient late_backup_graph;
    late_backup_graph.changes = {
        file("late-backup", "late-backup.txt", 4),
    };
    late_backup_graph.contents["late-backup"] = "data";
    late_backup_graph.before_download_write =
        [destination =
             late_backup_root / "late-backup.txt"](const std::string&) {
            std::ofstream output{destination};
            output << "user data";
        };
    FakeItemStore late_backup_items;
    FakeMetrics late_backup_metrics;
    auto late_backup_config = config_for(late_backup_root, false);
    late_backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            late_backup_config,
            late_backup_graph,
            late_backup_items,
            late_backup_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        late_backup_items.upsert_count != 1 ||
        !late_backup_items.applied_delta.blocked_upserts.empty() ||
        !late_backup_metrics.last_success) {
        return fail(
            "safeBackup mode did not preserve a download-time conflict"
        );
    }
    bool preserved_late_change = false;
    for (const auto& entry :
         std::filesystem::directory_iterator{late_backup_root}) {
        if (entry.path().filename().string().starts_with(
                "late-backup.safeBackup-"
            )) {
            std::ifstream input{entry.path()};
            const std::string contents{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}
            };
            preserved_late_change = contents == "user data";
        }
    }
    if (!preserved_late_change) {
        return fail("download-time local content was not backed up");
    }

    const auto symlink_root = temporary.path() / "symlink-root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(symlink_root);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, symlink_root / "linked");
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {file("linked", "linked/file.txt", 4)};
    symlink_graph.contents["linked"] = "data";
    FakeItemStore symlink_items;
    FakeMetrics symlink_metrics;
    const auto symlink_config = config_for(symlink_root, false);
    if (onedrive::sync::SyncEngine{
            symlink_config,
            symlink_graph,
            symlink_items,
            symlink_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        symlink_graph.download_count != 0 || symlink_items.apply_count != 1 ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict" ||
        std::filesystem::exists(outside / "file.txt") ||
        !symlink_metrics.last_success) {
        return fail("symbolic link conflict was not isolated safely");
    }

    const auto layout_root = temporary.path() / "layout-root";
    const auto layout_outside = temporary.path() / "layout-outside";
    std::filesystem::create_directories(layout_root);
    std::filesystem::create_directories(layout_outside);
    std::filesystem::create_directory_symlink(
        layout_outside, layout_root / "accounts"
    );
    FakeGraphClient layout_graph;
    FakeItemStore layout_items;
    FakeMetrics layout_metrics;
    const auto layout_config =
        config_for(layout_root / "accounts/Test-User/drives/Test-Drive", false);
    bool layout_symlink_rejected = false;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            layout_config,
            layout_graph,
            layout_items,
            layout_metrics,
            &default_console
        }
                              .synchronize());
    } catch (const std::runtime_error& error) {
        layout_symlink_rejected =
            std::string_view{error.what()}.contains("contains a symbolic link");
    }
    if (!layout_symlink_rejected ||
        std::filesystem::exists(layout_outside / "Test-User") ||
        layout_metrics.last_success) {
        return fail("symbolic link in account data layout was accepted");
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
                .synchronize() != 2 ||
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
                .synchronize() != 2 ||
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
                .synchronize() != 0 ||
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
                .synchronize() != 2 ||
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
                .synchronize() != 0 ||
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

int test_invalid_delta_cursor_restarts_full_query() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    FakeGraphClient graph;
    graph.reject_saved_cursor = true;
    graph.changes = {
        {
            .id = "directory",
            .name = "Documents",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "Documents",
            .directory = true,
        },
    };
    FakeItemStore items;
    items.saved_delta_link = "https://graph.example.test/delta?token=expired";
    FakeMetrics metrics;

    if (onedrive::sync::SyncEngine{
            config_for(root, false), graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.delta_requests !=
            std::vector<std::optional<std::string>>{
                items.saved_delta_link,
                std::nullopt,
            } ||
        items.apply_count != 1 ||
        items.applied_delta.apply_mode !=
            onedrive::storage::DeltaApplyMode::replace ||
        items.applied_delta.upserts.size() != 1 || !metrics.last_success) {
        return fail(
            "invalid delta cursor did not restart a replacing full query"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_engine_owns_configuration();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_dry_run_and_success(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_remote_content_tag_strategy();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_refreshes_delta_state();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_filter_policies_apply_in_both_directions();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_selective_sync_remote_moves();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_malware_file_is_blocked_without_overwriting_local_data();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_failure_and_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_blocked_items_continue_and_retry();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_delta_cursor_restarts_full_query();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
