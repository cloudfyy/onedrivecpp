#include "support.hpp"
#include "onedrive/util/sha256.hpp"

#include <chrono>

namespace {

using namespace onedrive::test::app;

std::int64_t modified_ticks(const std::filesystem::path& path) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::filesystem::last_write_time(path).time_since_epoch()
    ).count();
}

onedrive::storage::ItemState item(
    const std::filesystem::path& root,
    std::string remote_path,
    std::int64_t local_size,
    std::int64_t ticks
) {
    return {
        .drive_id = "drive-id",
        .remote_id = remote_path,
        .parent_id = "parent",
        .name = std::filesystem::path{remote_path}.filename().string(),
        .etag = "etag",
        .ctag = "ctag",
        .remote_path = remote_path,
        .local_path = root / remote_path,
        .last_modified = "2026-10-07T00:00:00Z",
        .size = local_size,
        .local_size = local_size,
        .local_modified_ticks = ticks,
    };
}

int test_inspection() {
    CliFixture fixture;
    if (fixture.authenticate().exit_code != 0) {
        return fail("inspection fixture authentication failed");
    }
    const auto empty_partials = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "partials",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (empty_partials.exit_code != 0 ||
        !empty_partials.standard_output.contains(
            R"("event":"no_partial_downloads")"
        ) ||
        !empty_partials.standard_output.contains(R"("recorded":"0")")) {
        return fail("inspect partials did not handle absent state");
    }

    const auto identity = onedrive::test::test_drive_identity("me");
    const auto root = onedrive::account::AccountState::drive_data_directory(
        fixture.sync_path, identity
    );
    std::filesystem::create_directories(root / "Documents");
    const auto account_paths =
        onedrive::account::AccountState::locate(fixture.state_path, identity);
    std::ofstream{account_paths.drive_directory / "items.sqlite3"};

    const auto ok_path = root / "Documents/ok.txt";
    {
        std::ofstream output{ok_path};
        output << "okay";
    }
    const auto modified_path = root / "Documents/modified.txt";
    {
        std::ofstream output{modified_path};
        output << "changed";
    }
    auto ok = item(root, "Documents/ok.txt", 4, modified_ticks(ok_path));
    ok.content_hash = onedrive::util::FileHash{
        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
        .value = onedrive::util::sha256_hex("okay"),
    };
    auto missing = item(root, "Documents/missing.txt", 7, 1);
    auto modified =
        item(root, "Documents/modified.txt", 1, modified_ticks(modified_path));
    const auto no_hash_path = root / "Documents/no-hash.txt";
    {
        std::ofstream output{no_hash_path};
        output << "plain";
    }
    auto no_hash =
        item(root, "Documents/no-hash.txt", 5, modified_ticks(no_hash_path));
    auto type_changed = item(root, "Documents/type-changed.txt", 0, 0);
    std::filesystem::create_directory(type_changed.local_path);
    auto outside_root = item(root, "Documents/outside.txt", 0, 0);
    outside_root.local_path = fixture.temporary_directory.path() / "outside.txt";
    fixture.runtime_factory.item_states = {
        ok, missing, modified, no_hash, type_changed, outside_root
    };

    const auto partial_path =
        root / "Documents/.archive.bin.onedrive-partial-1-1";
    {
        std::ofstream output{partial_path};
        output << "part";
    }
    auto partial_item = item(root, "Documents/archive.bin", 10, 0);
    partial_item.size = 10;
    auto missing_partial_item =
        item(root, "Documents/missing.bin", 12, 0);
    missing_partial_item.size = 12;
    auto type_changed_partial_item =
        item(root, "Documents/type-partial.bin", 8, 0);
    type_changed_partial_item.size = 8;
    const auto type_changed_partial_path =
        root / "Documents/.type-partial.bin.onedrive-partial-1-3";
    std::filesystem::create_directory(type_changed_partial_path);
    auto outside_partial_item =
        item(root, "Documents/outside-partial.bin", 8, 0);
    outside_partial_item.size = 8;
    outside_partial_item.local_path =
        fixture.temporary_directory.path() / "outside-partial.bin";
    auto mismatch_partial_item =
        item(root, "Documents/mismatch.bin", 8, 0);
    mismatch_partial_item.size = 8;
    auto invalid_partial_item =
        item(root, "Documents/invalid.bin", 3, 0);
    invalid_partial_item.size = 3;
    auto truncated_partial_item =
        item(root, "Documents/truncated.bin", 8, 0);
    truncated_partial_item.size = 8;
    const auto truncated_partial_path =
        root / "Documents/.truncated.bin.onedrive-partial-1-6";
    {
        std::ofstream output{truncated_partial_path};
        output << "xx";
    }
    fixture.runtime_factory.partial_download_states = {
        {
            .item = partial_item,
            .temporary_path = partial_path,
            .completed_bytes = 4,
        },
        {
            .item = missing_partial_item,
            .temporary_path =
                root /
                "Documents/.missing.bin.onedrive-partial-1-2",
            .completed_bytes = 3,
        },
        {
            .item = type_changed_partial_item,
            .temporary_path = type_changed_partial_path,
            .completed_bytes = 2,
        },
        {
            .item = outside_partial_item,
            .temporary_path =
                fixture.temporary_directory.path() /
                ".outside-partial.bin.onedrive-partial-1-4",
            .completed_bytes = 2,
        },
        {
            .item = mismatch_partial_item,
            .temporary_path = root / "Documents/not-a-partial.bin",
            .completed_bytes = 2,
        },
        {
            .item = invalid_partial_item,
            .temporary_path =
                root / "Documents/.invalid.bin.onedrive-partial-1-5",
            .completed_bytes = 4,
        },
        {
            .item = truncated_partial_item,
            .temporary_path = truncated_partial_path,
            .completed_bytes = 4,
        },
    };

    const auto storage = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "storage",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (storage.exit_code != 0 ||
        !storage.standard_output.contains(R"("event":"storage")") ||
        !storage.standard_output.contains(R"("tracked_files":"6")") ||
        !storage.standard_output.contains(R"("partial_downloads":"7")") ||
        !storage.standard_output.contains(R"("resumable_partials":"1")") ||
        !storage.standard_output.contains(R"("partial_bytes":"6 B")")) {
        return fail("inspect storage did not report local storage usage");
    }

    const auto partials = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "partials",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (partials.exit_code != 0 ||
        !partials.standard_output.contains(R"("status":"resumable")") ||
        !partials.standard_output.contains(R"("status":"missing")") ||
        !partials.standard_output.contains(R"("status":"type-changed")") ||
        !partials.standard_output.contains(R"("status":"outside-root")") ||
        !partials.standard_output.contains(R"("status":"path-mismatch")") ||
        !partials.standard_output.contains(
            R"("status":"invalid-checkpoint")"
        ) ||
        !partials.standard_output.contains(
            R"("event":"partial_download_summary")"
        ) ||
        !partials.standard_output.contains(R"("invalid":"6")")) {
        return fail("inspect partials did not classify partial downloads");
    }

    const auto partials_console = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "partials",
            "--config",
            fixture.config_path.string(),
            "--ui",
            "console",
        },
        "untouched\n"
    );
    if (partials_console.exit_code != 0 ||
        !partials_console.standard_output.contains("recorded:     7") ||
        !partials_console.standard_output.contains("invalid:      6") ||
        partials_console.standard_output.contains("Press Enter to exit")) {
        return fail(
            "partial console output lost its summary or became interactive"
        );
    }
    for (const auto& partial :
         fixture.runtime_factory.partial_download_states) {
        if (!partials.standard_output.contains(partial.item.remote_path) ||
            !partials_console.standard_output.contains(partial.item.remote_path
            ) ||
            !partials_console.standard_output.contains(
                partial.temporary_path.string()
            )) {
            return fail("partial console or JSON output omitted a saved file");
        }
    }

    const auto files = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "files",
            "Documents",
            "--status",
            "missing",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (files.exit_code != 0 ||
        !files.standard_output.contains(R"("remote_path":"Documents/missing.txt")"
        ) ||
        files.standard_output.contains(R"("remote_path":"Documents/ok.txt")") ||
        !files.standard_output.contains(
            R"("event":"downloaded_file_summary")"
        ) ||
        !files.standard_output.contains(R"("inspected":"6")") ||
        !files.standard_output.contains(R"("shown":"1")") ||
        !files.standard_output.contains(R"("modified":"1")") ||
        !files.standard_output.contains(R"("type_changed":"1")") ||
        !files.standard_output.contains(R"("outside_root":"1")")) {
        return fail("inspect files did not filter downloaded file status");
    }

    const auto invalid_path = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "files",
            "../outside",
            "--config",
            fixture.config_path.string(),
        }
    );
    if (invalid_path.exit_code != 1 ||
        !invalid_path.standard_error.contains("invalid component")) {
        return fail("inspect files accepted an unsafe Drive-relative path");
    }

    const auto verified = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "verify",
            "Documents/ok.txt",
            "--mode",
            "content",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (verified.exit_code != 0 ||
        !verified.standard_output.contains(R"("result":"verified")") ||
        !verified.standard_output.contains(R"("algorithm":"sha256")")) {
        return fail("inspect verify did not validate saved content hash");
    }
    fixture.runtime_factory.item_states[0].content_hash->value = "incorrect";
    const auto mismatch = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "verify",
            "Documents/ok.txt",
            "--mode",
            "content",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (mismatch.exit_code != 1 ||
        !mismatch.standard_output.contains(R"("result":"hash-mismatch")")) {
        return fail("inspect verify accepted mismatched content");
    }
    const auto unavailable = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "verify",
            "Documents/no-hash.txt",
            "--mode",
            "content",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    const auto metadata_verified = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "verify",
            "Documents/no-hash.txt",
            "--mode",
            "metadata",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (unavailable.exit_code != 1 ||
        !unavailable.standard_output.contains(
            R"("result":"hash-unavailable")"
        ) ||
        metadata_verified.exit_code != 0 ||
        !metadata_verified.standard_output.contains(
            R"("result":"verified")"
        )) {
        return fail("inspect verify did not distinguish verification modes");
    }

    const auto effective_config = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "config",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (effective_config.exit_code != 0 ||
        !effective_config.standard_output.contains(
            R"("event":"effective_config")"
        ) ||
        !effective_config.standard_output.contains(
            R"("event":"effective_transfer_config")"
        ) ||
        !effective_config.standard_output.contains(R"("proxy":"disabled")") ||
        effective_config.standard_output.contains("test-application")) {
        return fail("inspect config exposed secrets or omitted effective data");
    }

    const auto orphan =
        root / "Documents/.orphan.bin.onedrive-partial-9-9";
    {
        std::ofstream output{orphan};
        output << "orphan";
    }
    const auto cleanup_plan = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "cleanup",
            "--dry-run",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (cleanup_plan.exit_code != 0 ||
        !cleanup_plan.standard_output.contains(R"("mode":"dry-run")") ||
        !cleanup_plan.standard_output.contains(R"("orphaned_files":"1")") ||
        !std::filesystem::exists(orphan) ||
        !std::filesystem::exists(truncated_partial_path)) {
        return fail("state cleanup dry-run changed partial state");
    }
    const auto unconfirmed_cleanup = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "cleanup",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (unconfirmed_cleanup.exit_code != 1 ||
        !unconfirmed_cleanup.standard_error.contains(
            R"("event":"confirmation_required")"
        ) ||
        !std::filesystem::exists(orphan)) {
        return fail("state cleanup bypassed JSON confirmation");
    }
    const auto cleanup = run_application(
        fixture.runtime_factory,
        {
            "onedrive-cpp",
            "state",
            "cleanup",
            "--yes",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json",
        }
    );
    if (cleanup.exit_code != 0 ||
        !cleanup.standard_output.contains(R"("event":"cleanup_completed")") ||
        std::filesystem::exists(orphan) ||
        std::filesystem::exists(truncated_partial_path) ||
        !std::filesystem::exists(type_changed_partial_path)) {
        return fail("state cleanup did not safely remove partial files");
    }
    return 0;
}

} // namespace

int main() {
    try {
        return test_inspection();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
