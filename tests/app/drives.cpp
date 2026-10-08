#include "support.hpp"

#include <nlohmann/json.hpp>

namespace {

using namespace onedrive::test::app;
using onedrive::test::write_file;
using Json = nlohmann::json;

Json drive_results(const RunResult& result) {
    if (result.exit_code != 0) {
        throw std::runtime_error(result.standard_error);
    }
    Json drives = Json::object();
    std::istringstream input{result.standard_output};
    for (std::string line; std::getline(input, line);) {
        const auto event = Json::parse(line);
        if (event.at("event") == "drive") {
            const auto& values = event.at("values");
            const auto id = values.at("id").get<std::string>();
            if (drives.contains(id)) {
                throw std::runtime_error("drive listing repeated a Drive ID");
            }
            drives[id] = values;
        }
    }
    return drives;
}

int test_drive_statistics() {
    CliFixture fixture;
    if (fixture.authenticate().exit_code != 0) {
        return fail("cannot authenticate drive inspection fixture");
    }
    const auto run = [&] {
        return run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "inspect",
             "drives",
             "--config",
             fixture.config_path.string(),
             "--output",
             "json"}
        );
    };
    const auto missing = drive_results(run());
    const auto& configured = missing.at("drive-id");
    const auto& shared = missing.at("shared-drive-id");
    if (configured.at("reference") != "me" ||
        configured.at("configured") != "true" ||
        configured.at("total") != "2.00 KiB" ||
        configured.at("used") != "400 B" ||
        configured.at("remaining") != "600 B" ||
        configured.at("deleted") != "25 B" ||
        configured.at("state") != "normal" ||
        configured.at("known_files") != "unavailable" ||
        configured.at("downloaded_files") != "unavailable" ||
        configured.at("statistics_source") !=
            "local state (not cloud totals)" ||
        shared.contains("reference") || shared.at("total") != "unavailable" ||
        fixture.runtime_factory.item_store_count != 0 ||
        std::filesystem::exists(fixture.sync_path)) {
        return fail(
            "drive references, missing statistics or capacities were misleading"
        );
    }

    const auto identity = onedrive::test::test_drive_identity("me");
    const auto paths =
        onedrive::account::AccountState::locate(fixture.state_path, identity);
    write_file(paths.drive_directory / "items.sqlite3", "");
    const auto root = onedrive::account::AccountState::drive_data_directory(
        fixture.sync_path, identity
    );
    std::filesystem::create_directories(root);
    const auto make_item = [&](const std::string& name,
                               bool directory = false) {
        return onedrive::storage::ItemState{
            .drive_id = identity.drive_id,
            .remote_id = name,
            .name = name,
            .remote_path = name,
            .local_path = root / name,
            .content_hash = std::nullopt,
            .directory = directory,
        };
    };
    auto empty = make_item("empty");
    write_file(empty.local_path, "");
    auto changed = make_item("changed");
    write_file(changed.local_path, "modified contents");
    auto absent = make_item("absent");
    auto directory = make_item("folder", true);
    std::filesystem::create_directory(directory.local_path);
    auto symlink = make_item("symlink");
    std::filesystem::create_symlink(changed.local_path, symlink.local_path);
    auto outside = make_item("outside");
    outside.local_path = fixture.temporary_directory.path() / "outside";
    write_file(outside.local_path, "outside");
    auto type_changed = make_item("type-changed");
    std::filesystem::create_directory(type_changed.local_path);
    auto other_drive = make_item("other-drive");
    other_drive.drive_id = "shared-drive-id";
    fixture.runtime_factory.item_states = {
        empty,
        changed,
        absent,
        directory,
        symlink,
        outside,
        type_changed,
        other_drive
    };
    const auto queued = make_item("queued");
    fixture.runtime_factory.pending_download_states = {
        {.item = queued},
        {.item = changed},
        {.item = directory},
    };
    fixture.runtime_factory.partial_download_states = {
        {.item = queued},
        {.item = make_item("partial")},
        {.item = directory},
    };
    fixture.runtime_factory.blocked_item_states = {
        {.drive_id = identity.drive_id, .remote_id = "blocked"},
        {.drive_id = identity.drive_id, .remote_id = changed.remote_id},
        {.drive_id = identity.drive_id,
         .remote_id = "blocked-folder",
         .directory = true},
        {.drive_id = identity.drive_id,
         .remote_id = "deleted",
         .deleted = true},
        {.drive_id = "shared-drive-id", .remote_id = "other-blocked"},
    };
    const auto populated = drive_results(run());
    const auto& stats = populated.at("drive-id");
    if (stats.at("known_files") != "9" || stats.at("downloaded_files") != "2" ||
        stats.at("pending_files") != "3" || stats.at("blocked_files") != "2" ||
        populated.at("shared-drive-id").at("known_files") != "unavailable" ||
        fixture.runtime_factory.item_store_apply_delta_count != 0 ||
        fixture.runtime_factory.item_store_reset_count != 0) {
        return fail(
            "drive statistics double-counted IDs or included "
            "directories/unsafe paths"
        );
    }
    fixture.runtime_factory.include_configured_drive = true;
    if (drive_results(run()) != populated) {
        return fail(
            "an already-listed configured Drive changed inspection output"
        );
    }
    auto shared_identity = identity;
    shared_identity.drive_id = "shared-drive-id";
    shared_identity.configured_drive_id = "shared-drive-id";
    shared_identity.drive_name = "Shared Drive";
    const auto shared_paths = onedrive::account::AccountState::locate(
        fixture.state_path, shared_identity
    );
    std::filesystem::create_directories(shared_paths.drive_directory);
    write_file(shared_paths.drive_directory / "items.sqlite3", "");
    const auto both = drive_results(run());
    if (both.at("shared-drive-id").at("known_files") != "2" ||
        both.at("shared-drive-id").at("downloaded_files") != "0" ||
        both.at("drive-id").at("known_files") != "9") {
        return fail("drive statistics were mixed across Drive IDs");
    }
    fixture.runtime_factory.item_states.clear();
    fixture.runtime_factory.pending_download_states.clear();
    fixture.runtime_factory.partial_download_states.clear();
    fixture.runtime_factory.blocked_item_states.clear();
    const auto zero = drive_results(run());
    if (zero.at("drive-id").at("known_files") != "0" ||
        zero.at("drive-id").at("downloaded_files") != "0") {
        return fail("an empty known database was treated as unavailable");
    }
    auto config = onedrive::test::read_file(fixture.config_path);
    config.insert(config.find("[sync]\n") + 7, "drive_id = \"drive-id\"\n");
    write_file(fixture.config_path, config);
    const auto explicit_id = drive_results(run());
    if (explicit_id.at("drive-id").at("reference") != "drive-id" ||
        explicit_id.at("shared-drive-id").contains("reference")) {
        return fail("an explicit Drive reference was falsely labelled me");
    }
    std::filesystem::remove(paths.drive_directory / "items.sqlite3");
    std::filesystem::create_symlink(
        outside.local_path, paths.drive_directory / "items.sqlite3"
    );
    const auto unsafe = run();
    if (unsafe.exit_code != 1 ||
        !unsafe.standard_error.contains("not a regular file") ||
        onedrive::test::read_file(outside.local_path) != "outside") {
        return fail("drive statistics followed an unsafe database path");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_drive_statistics();
}
