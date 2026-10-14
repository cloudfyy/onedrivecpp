#include "support.hpp"
#include "onedrive/logging/logging.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/file.h>

namespace {

using namespace onedrive::test::app;
namespace exit_code = onedrive::exit_code;
using Json = nlohmann::json;

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path state{temporary.path() / "state"};
    std::filesystem::path config{temporary.path() / "config.toml"};
    onedrive::account::DriveIdentity identity{
        onedrive::test::test_drive_identity("me")
    };
    onedrive::account::AccountPaths paths;
    FakeRuntimeFactory factory;

    Fixture() {
        write_config("me");
    }

    void write_config(std::string_view drive) const {
        onedrive::test::write_file(
            config,
            "config_version = 2\n[state]\ndirectory = \"" + state.string() +
                "\"\n[sync]\ndrive_id = \"" + std::string{drive} +
                "\"\ndata_directory = \"" +
                (temporary.path() / "unused-sync").string() +
                "\"\n[auth]\napplication_id = \"\"\n"
        );
    }

    void activate() {
        std::filesystem::create_directory(state);
        paths = onedrive::account::AccountState::activate(
            state, identity, "saved-test-token"
        );
        std::filesystem::remove(paths.account_directory / "refresh_token");
    }

    void seed() {
        const onedrive::logging::Session logging{{.level = "off"}};
        activate();
        onedrive::storage::ItemDatabase database{
            paths.drive_directory, identity
        };
        database.open();
        struct Record {
            const char* path;
            const char* reason;
            const char* message;
        };
        constexpr std::array records{
            Record{
                "Documents/notes.one", "unsupported_onenote", "OneNote file"
            },
            Record{
                "Documents/local.txt", "local_modification", "Local changes"
            },
            Record{"remove.txt", "upload_delete_limit", "Deletion safeguard"},
            Record{"quota.txt", "upload_remote_quota", "Low storage 空间不足"},
            Record{"unknown.txt", "future_reason", "New blocker reason"},
        };
        for (const auto& record : records) {
            onedrive::storage::BlockedItem item;
            item.drive_id = identity.drive_id;
            item.remote_id = record.reason;
            item.remote_path = record.path;
            item.reason_code = record.reason;
            item.reason_message = record.message;
            database.save_blocked_item(item);
            if (item.reason_code == "unsupported_onenote") {
                database.save_blocked_item(item);
            }
        }
    }

    RunResult run(std::vector<std::string> options = {}) {
        std::vector<std::string> arguments{
            "onedrive-cpp",
            "inspect",
            "blocked",
            "--config",
            config.string(),
            "--ui",
            "console"
        };
        arguments.insert(arguments.end(), options.begin(), options.end());
        return run_application(factory, std::move(arguments));
    }
};

Json events(const RunResult& result) {
    Json parsed = Json::array();
    std::istringstream input{result.standard_output};
    std::string line;
    while (std::getline(input, line)) {
        parsed.push_back(Json::parse(line));
    }
    return parsed;
}

int test_empty_state() {
    Fixture missing;
    const auto result = missing.run({"--output", "json"});
    const auto parsed = events(result);
    if (result.exit_code != exit_code::success || parsed.size() != 2 ||
        parsed[0]["event"] != "no_blocked_items" ||
        parsed[1]["event"] != "blocked_item_summary" ||
        parsed[1]["values"]["recorded"] != "0" ||
        parsed[1]["values"]["shown"] != "0" ||
        std::filesystem::exists(missing.state)) {
        return fail("offline blocked inspection created missing state");
    }
    Fixture empty;
    {
        const onedrive::logging::Session logging{{.level = "off"}};
        empty.activate();
    }
    const auto no_database = empty.run();
    if (no_database.exit_code != exit_code::success ||
        !no_database.standard_output.contains("No saved blocked items.") ||
        std::filesystem::exists(
            empty.paths.drive_directory / "items.sqlite3"
        ) ||
        empty.factory.token_store_count != 0 ||
        empty.factory.graph_client_count != 0 ||
        empty.factory.graph_info_client_count != 0) {
        return fail("empty offline inspection created a database or used auth");
    }
    return EXIT_SUCCESS;
}

int test_records_and_read_only_behavior() {
    Fixture fixture;
    fixture.seed();
    const auto database = fixture.paths.drive_directory / "items.sqlite3";
    const auto before = onedrive::test::read_file(database);
    const auto modified = std::filesystem::last_write_time(database);
    const auto state_paths = [&] {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(fixture.state)) {
            paths.push_back(entry.path());
        }
        std::ranges::sort(paths);
        return paths;
    };
    const auto saved_paths = state_paths();
    const auto text = fixture.run();
    if (text.exit_code != exit_code::success ||
        !text.standard_output.contains("Documents/notes.one") ||
        !text.standard_output.contains("recorded discoveries: 2") ||
        !text.standard_output.contains("future_reason")) {
        return fail("text inspection omitted blocked-item details");
    }
    for (const auto& drive :
         {fixture.identity.configured_drive_id, fixture.identity.drive_id}) {
        fixture.write_config(drive);
        const auto result = fixture.run({"--output", "json"});
        const auto parsed = events(result);
        if (result.exit_code != exit_code::success || parsed.size() != 6 ||
            parsed.back()["event"] != "blocked_item_summary" ||
            parsed.back()["values"]["recorded"] != "5" ||
            parsed.back()["values"]["shown"] != "5") {
            return fail("JSON inspection lost configured/canonical Drive data");
        }
        for (std::size_t index = 0; index < 5; ++index) {
            const auto& record = parsed[index];
            const auto& values = record["values"];
            const auto reason = values["reason_code"].get<std::string>();
            const auto expected_control = reason == "local_modification"
                                              ? "local_conflict_configuration"
                                          : reason == "upload_delete_limit"
                                              ? "remote_deletion_configuration"
                                              : "fixed";
            if (record["event"] != "blocked_item" ||
                values["drive_id"] != fixture.identity.drive_id ||
                values["remote_id"] != reason ||
                values["policy_control"] != expected_control ||
                values["attempt_count"] !=
                    (reason == "unsupported_onenote" ? "2" : "1") ||
                ((reason == "unsupported_onenote" ||
                  reason == "future_reason") &&
                 values["recovery_allowed"] != "false") ||
                (reason == "upload_remote_quota" &&
                 values["recovery_allowed"] != "true")) {
                return fail("JSON details disagreed with saved facts/strategy");
            }
        }
    }
    const auto quiet = fixture.run({"--output", "json", "--quiet"});
    fixture.write_config("unrelated-drive");
    const auto unrelated = fixture.run({"--output", "json"});
    if (quiet.exit_code != exit_code::success ||
        !quiet.standard_output.empty() || !quiet.standard_error.empty()) {
        return fail("quiet inspection produced output");
    }
    if (unrelated.exit_code != exit_code::success ||
        events(unrelated).back()["values"]["recorded"] != "0") {
        return fail("inspection selected an unrelated Drive");
    }
    if (before != onedrive::test::read_file(database) ||
        modified != std::filesystem::last_write_time(database)) {
        return fail("blocked inspection mutated the saved database");
    }
    for (const auto& path : state_paths()) {
        // WAL readers may create SQLite coordination files, but not app state.
        if (std::ranges::find(saved_paths, path) == saved_paths.end() &&
            path != database.string() + "-shm" &&
            path != database.string() + "-wal") {
            return fail(
                "blocked inspection created unexpected state: " + path.string()
            );
        }
    }
    if (std::filesystem::exists(fixture.temporary.path() / "unused-sync") ||
        fixture.factory.token_store_count != 0 ||
        fixture.factory.graph_client_count != 0 ||
        fixture.factory.graph_info_client_count != 0 ||
        fixture.factory.item_store_count != 0) {
        return fail(
            "blocked inspection mutated state or performed online work"
        );
    }
    const onedrive::util::UniqueFD lock{::open(
        (fixture.state / "onedrive-cpp.lock").c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        0600
    )};
    if (!lock || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        return fail("cannot hold the fixture synchronization lock");
    }
    fixture.write_config("me");
    const auto concurrent = fixture.run({"--output", "json"});
    if (concurrent.exit_code != exit_code::success ||
        events(concurrent).back()["values"]["recorded"] != "5") {
        return fail(
            "read-only inspection was blocked by the synchronization lock"
        );
    }
    return EXIT_SUCCESS;
}

int test_filters() {
    Fixture fixture;
    fixture.seed();
    const std::vector<std::pair<std::vector<std::string>, std::string>> cases{
        {{"--reason", "unsupported_onenote"}, "1"},
        {{"--reason", "future_reason"}, "1"},
        {{"--reason", "unsupported"}, "0"},
        {{"--reason", "FUTURE_REASON"}, "0"},
        {{"--search", "DOCUMENTS/"}, "2"},
        {{"--search", "UNSUPPORTED_ONENOTE"}, "1"},
        {{"--search", "LOW STORAGE"}, "1"},
        {{"--search", "空间不足"}, "1"},
        {{"--search", "does-not-exist"}, "0"},
        {{"--reason", "unsupported_onenote", "--search", "local"}, "0"},
    };
    for (const auto& [options, shown] : cases) {
        auto arguments = options;
        arguments.insert(arguments.end(), {"--output", "json"});
        const auto result = fixture.run(arguments);
        const auto parsed = events(result);
        if (result.exit_code != exit_code::success ||
            parsed.back()["values"]["recorded"] != "5" ||
            parsed.back()["values"]["shown"] != shown ||
            (shown == "0" &&
             (parsed.size() != 2 || parsed[0]["event"] != "no_blocked_items" ||
              parsed[0]["message"] !=
                  "No saved blocked items match the filters."))) {
            return fail(
                "reason/search filter or filtered totals were incorrect"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_query_errors() {
    for (const bool database_error : {false, true}) {
        Fixture fixture;
        fixture.seed();
        onedrive::test::write_file(
            fixture.paths.drive_directory /
                (database_error ? "items.sqlite3" : "drive.json"),
            "invalid"
        );
        const auto result = fixture.run({"--output", "json"});
        if (result.exit_code != exit_code::failure ||
            !result.standard_error.contains(
                database_error ? "database" : "metadata"
            ) ||
            result.standard_output.contains("no_blocked_items") ||
            result.standard_output.contains("blocked_item_summary")) {
            return fail(
                "query failure appeared as successful empty inspection"
            );
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    try {
        if (test_empty_state() != EXIT_SUCCESS ||
            test_records_and_read_only_behavior() != EXIT_SUCCESS ||
            test_filters() != EXIT_SUCCESS ||
            test_query_errors() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
