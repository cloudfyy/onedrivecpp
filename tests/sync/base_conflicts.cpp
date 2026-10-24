#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/ui/cli/backend.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "base_cases.hpp"

namespace onedrive::test::sync::base_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

namespace {

class ProgressBackend final : public onedrive::cli::ConsoleBackend {
public:
    void emit(const onedrive::cli::ConsoleEvent& event) override {
        if (const auto* progress =
                std::get_if<onedrive::cli::DownloadProgressEvent>(&event)) {
            downloads.push_back(*progress);
        }
    }

    bool confirm(const onedrive::cli::ConfirmationRequest&) override {
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

} // namespace

int test_download_failure_preserves_completed_items() {
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
    return EXIT_SUCCESS;
}

int test_independent_download_reuse() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
    const onedrive::cli::Console progress_console{std::move(progress_backend)};
    if (onedrive::sync::SyncEngine{
            independent_config,
            independent_graph,
            independent_items,
            independent_metrics,
            &progress_console
        }
                .synchronize() != exit_code::success ||
        independent_graph.download_count.load(std::memory_order_relaxed) != 1 ||
        captured_progress->downloads.size() < 2 ||
        captured_progress->downloads.front().completed_files != 1 ||
        captured_progress->downloads.front().file_count != 2 ||
        captured_progress->downloads.front().downloaded != 4 ||
        captured_progress->downloads.front().total != 8 ||
        captured_progress->downloads.back().completed_files != 2 ||
        captured_progress->downloads.back().file_count != 2 ||
        !std::filesystem::exists(independent_root / "failed-first.txt") ||
        !std::filesystem::exists(independent_root / "completed-second.txt") ||
        !independent_metrics.last_success) {
        return fail(
            "retry downloaded a file that completed before interruption"
        );
    }
    independent_graph.download_count.store(0, std::memory_order_relaxed);
    auto reused_backend = std::make_unique<ProgressBackend>();
    auto* captured_reused = reused_backend.get();
    const onedrive::cli::Console reused_console{std::move(reused_backend)};
    if (onedrive::sync::SyncEngine{
            independent_config,
            independent_graph,
            independent_items,
            independent_metrics,
            &reused_console
        }
                .synchronize() != exit_code::success ||
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
    return EXIT_SUCCESS;
}

int test_download_time_conflict() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
                .synchronize() != exit_code::blocked ||
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
    return EXIT_SUCCESS;
}

int test_local_download_conflict() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
                .synchronize() != exit_code::blocked ||
        conflict_graph.download_count != 0 || conflict_items.apply_count != 1 ||
        conflict_items.applied_delta.blocked_upserts.size() != 1 ||
        conflict_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !conflict_metrics.last_success) {
        return fail("local conflict was not isolated and persisted");
    }
    return EXIT_SUCCESS;
}

int test_download_conflict_backup() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
                .synchronize() != exit_code::success ||
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
    return EXIT_SUCCESS;
}

int test_download_time_backup() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
                .synchronize() != exit_code::success ||
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
    return EXIT_SUCCESS;
}

int test_download_parent_symlink() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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
                .synchronize() != exit_code::blocked ||
        symlink_graph.download_count != 0 || symlink_items.apply_count != 1 ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict" ||
        std::filesystem::exists(outside / "file.txt") ||
        !symlink_metrics.last_success) {
        return fail("symbolic link conflict was not isolated safely");
    }
    return EXIT_SUCCESS;
}

int test_account_layout_symlink() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;
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

} // namespace onedrive::test::sync::base_cases
