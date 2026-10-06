#include "support.hpp"

namespace {

using namespace onedrive::test::download;

int test_single_file_download_coordination() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "single";
    auto config = onedrive::config::Config::defaults();
    config.sync_directory = root;
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
    unmounted_config.sync_mount_point = temporary.path();
    try {
        static_cast<void>(onedrive::sync::plan_single_file_download(
            unmounted_config, "Documents/single.txt", graph, console
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
            dry_config, "Documents/single.txt", graph, console
        ) != 0 ||
        graph_implementation.download_count != 0 ||
        std::filesystem::exists(root / "Documents/single.txt")) {
        return fail("single-file dry run changed local state");
    }

    if (onedrive::sync::download_single_file(
            config, "Documents/single.txt", graph, items, console
        ) != 0 ||
        graph_implementation.download_count != 1 ||
        !std::filesystem::is_regular_file(root / "Documents/single.txt") ||
        !item_implementation.states.contains("single")) {
        return fail("single-file download did not reuse the safe transaction");
    }
    if (onedrive::sync::download_single_file(
            config, "Documents/single.txt", graph, items, console
        ) != 0 ||
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
    if (graph_implementation.download_count != 1) {
        return fail("rejected single-file target started a download");
    }

    auto backup_config = config;
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::download_single_file(
            backup_config, "conflict.txt", graph, items, console
        ) != 0 ||
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
        ) != 0) {
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
    return test_single_file_download_coordination();
}
