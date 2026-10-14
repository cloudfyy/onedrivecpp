#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/ui/cli/console.hpp"

#include <algorithm>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::download;

int test_unsupported_single_downloads() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "single";
    std::filesystem::create_directories(root / "Notebook");
    onedrive::test::write_file(root / "notes.one", "keep notes");
    onedrive::test::write_file(root / "Notebook/local.bin", "keep package");
    auto config = onedrive::config::Config::defaults();
    config.sync_data_directory = root;
    config.drive_id = "drive-id";
    FakeGraphClient implementation;
    FakeItemStore store;
    onedrive::graph::GraphClient graph{
        onedrive::util::borrowed_proxy, implementation
    };
    onedrive::storage::ItemStore items{onedrive::util::borrowed_proxy, store};
    std::ostringstream output;
    std::ostringstream error;
    const onedrive::cli::Console console{{}, output, error};
    for (const bool directory : {false, true}) {
        const std::string path = directory ? "Notebook" : "notes.one";
        implementation.lookup_item = remote_item("notes", path);
        implementation.lookup_item.directory = directory;
        implementation.lookup_item.unsupported_format = "onenote";
        implementation.lookup_item.validate_content = false;
        if (onedrive::sync::plan_single_file_download(
                config, path, graph, items, console
            ) != exit_code::blocked ||
            onedrive::sync::download_single_file(
                config, path, graph, items, console
            ) != exit_code::blocked) {
            return fail("unsupported single-file request was accepted");
        }
    }
    if (implementation.download_count != 0 || !store.states.empty() ||
        store.blocked.size() != 1 ||
        store.blocked[0].reason_code != "unsupported_onenote" ||
        onedrive::test::read_file(root / "notes.one") != "keep notes" ||
        onedrive::test::read_file(root / "Notebook/local.bin") !=
            "keep package" ||
        !(output.str() + error.str()).contains("unsupported_onenote")) {
        return fail(
            "unsupported single download changed files/state or hid its reason"
        );
    }
    implementation.lookup_item = remote_item("unknown", "mystery.txt");
    onedrive::storage::BlockedItem unknown;
    unknown.drive_id = config.drive_id;
    unknown.remote_id = "unknown";
    unknown.remote_path = "mystery.txt";
    unknown.reason_code = "future_reason";
    unknown.reason_message = "Unrecognized policy";
    store.save_blocked_item(unknown);
    if (onedrive::sync::plan_single_file_download(
            config, "mystery.txt", graph, items, console
        ) != exit_code::blocked ||
        onedrive::sync::download_single_file(
            config, "mystery.txt", graph, items, console
        ) != exit_code::blocked ||
        implementation.download_count != 0 ||
        std::filesystem::exists(root / "mystery.txt")) {
        return fail("single-file request bypassed an unknown blocked policy");
    }
    return EXIT_SUCCESS;
}

int test_single_file_download_coordination() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "single";
    auto config = onedrive::config::Config::defaults();
    config.sync_data_directory = root;
    config.drive_id = "drive-id";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
    FakeGraphClient graph_implementation;
    graph_implementation.lookup_item =
        remote_item("single", "Documents/single.txt");
    FakeItemStore item_implementation;
    onedrive::graph::GraphClient graph{
        onedrive::util::borrowed_proxy, graph_implementation
    };
    onedrive::storage::ItemStore items{
        onedrive::util::borrowed_proxy, item_implementation
    };
    std::ostringstream output;
    std::ostringstream error;
    const onedrive::cli::Console console{{}, output, error};

    auto unmounted_config = config;
    unmounted_config.sync_data_mount_point = temporary.path();
    try {
        static_cast<void>(onedrive::sync::plan_single_file_download(
            unmounted_config, "Documents/single.txt", graph, items, console
        ));
        return fail("single-file plan accepted an unmounted sync disk");
    } catch (const std::runtime_error& exception) {
        if (!std::string{exception.what()}.contains(
                "not currently mounted"
            )) {
            throw;
        }
    }

    auto dry_config = config;
    dry_config.dry_run = true;
    if (onedrive::sync::plan_single_file_download(
            dry_config, "Documents/single.txt", graph, items, console
        ) != exit_code::success ||
        graph_implementation.download_count != 0 ||
        std::filesystem::exists(root / "Documents/single.txt")) {
        return fail("single-file dry run changed local state");
    }

    if (onedrive::sync::download_single_file(
            config, "Documents/single.txt", graph, items, console
        ) != exit_code::success ||
        graph_implementation.download_count != 1 ||
        !std::filesystem::is_regular_file(root / "Documents/single.txt") ||
        !item_implementation.states.contains("single")) {
        return fail("single-file download did not reuse the safe transaction");
    }
    if (onedrive::sync::download_single_file(
            config, "Documents/single.txt", graph, items, console
        ) != exit_code::success ||
        graph_implementation.download_count != 1) {
        return fail("current single-file download was not reused");
    }

    graph_implementation.lookup_item = remote_item("directory", "Documents");
    graph_implementation.lookup_item.directory = true;
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config, "Documents", graph, items, console
        ));
        return fail("single-file download accepted a directory");
    } catch (const std::runtime_error&) {
    }

    graph_implementation.lookup_item = remote_item("malware", "malware.exe");
    graph_implementation.lookup_item.malware = true;
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config, "malware.exe", graph, items, console
        ));
        return fail("single-file download accepted Graph malware");
    } catch (const std::runtime_error&) {
    }

    graph_implementation.lookup_item = remote_item("conflict", "conflict.txt");
    std::filesystem::create_directories(root);
    {
        std::ofstream local{root / "conflict.txt"};
        local << "user data";
    }
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config, "conflict.txt", graph, items, console
        ));
        return fail("single-file download overwrote an untracked local file");
    } catch (const onedrive::sync::detail::LocalModificationConflictError&) {
    }
    if (std::ranges::none_of(item_implementation.blocked, [](const auto& item) {
            return item.remote_id == "conflict" &&
                   item.reason_code == "local_modification";
        })) {
        return fail("single-file conflict was not persisted");
    }
    if (graph_implementation.download_count != 1) {
        return fail("rejected single-file target started a download");
    }

    auto backup_config = config;
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::download_single_file(
            backup_config, "conflict.txt", graph, items, console
        ) != exit_code::success ||
        graph_implementation.download_count != 2) {
        return fail("single-file safeBackup download did not succeed");
    }
    std::filesystem::path conflict_backup;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                "conflict.safeBackup-"
            )) {
            conflict_backup = entry.path();
        }
    }
    std::ifstream installed_input{root / "conflict.txt"};
    std::ifstream backup_input{conflict_backup};
    const std::string installed_contents{
        std::istreambuf_iterator<char>{installed_input},
        std::istreambuf_iterator<char>{}
    };
    const std::string backup_contents{
        std::istreambuf_iterator<char>{backup_input},
        std::istreambuf_iterator<char>{}
    };
    if (conflict_backup.empty() || installed_contents != "data" ||
        backup_contents != "user data" ||
        !output.str().contains("Preserved local conflict")) {
        return fail("single-file safeBackup did not preserve local content");
    }

    graph_implementation.lookup_item =
        remote_item("identical", "identical.txt");
    {
        std::ofstream local{root / "identical.txt", std::ios::binary};
        local << "data";
    }
    struct stat before{};
    if (::stat((root / "identical.txt").c_str(), &before) == -1) {
        return fail("content-identical fixture could not be inspected");
    }
    if (onedrive::sync::download_single_file(
            backup_config, "identical.txt", graph, items, console
        ) != exit_code::success) {
        return fail("content-identical local file was not adopted");
    }
    struct stat after{};
    if (::stat((root / "identical.txt").c_str(), &after) == -1 ||
        before.st_ino != after.st_ino) {
        return fail("content-identical local file inode was replaced");
    }
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                "identical.safeBackup-"
            )) {
            return fail("content-identical local file created a safeBackup");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_unsupported_single_downloads() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_single_file_download_coordination();
}
