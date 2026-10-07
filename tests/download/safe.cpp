#include "support.hpp"

namespace {

using namespace onedrive::test::download;

int test_safe() {
    namespace detail = onedrive::sync::detail;
    DownloadFixture fixture;
    const auto& root = fixture.root;
    const auto& metadata = fixture.metadata;
    const auto& safe_root = fixture.safe_root;
    auto& space = fixture.space;
    auto& graph = fixture.graph;
    auto& items = fixture.items;
    if (safe_root.remove(
            root / "missing.txt",
            onedrive::sync::detail::FilesystemItemKind::file
        )) {
        return fail("safe removal reported a missing file as removed");
    }
    const auto removable_file = root / "remove.txt";
    {
        std::ofstream output{removable_file, std::ios::binary};
        output << "remove";
    }
    if (!safe_root.remove(
            removable_file,
            onedrive::sync::detail::FilesystemItemKind::file
        ) ||
        std::filesystem::exists(removable_file)) {
        return fail("safe removal did not remove a regular file");
    }
    const auto removable_directory = root / "remove-directory";
    std::filesystem::create_directory(removable_directory);
    if (!safe_root.remove(
            removable_directory,
            onedrive::sync::detail::FilesystemItemKind::directory
        ) ||
        std::filesystem::exists(removable_directory)) {
        return fail("safe removal did not remove an empty directory");
    }
    const auto nonempty_directory = root / "nonempty-directory";
    std::filesystem::create_directory(nonempty_directory);
    {
        std::ofstream output{nonempty_directory / "local.txt"};
        output << "local";
    }
    try {
        static_cast<void>(safe_root.remove(
            nonempty_directory,
            onedrive::sync::detail::FilesystemItemKind::directory
        ));
        return fail("safe removal accepted a non-empty directory");
    } catch (const detail::SafePathConflictError&) {
    }
    const auto removal_symlink = root / "remove-link";
    std::filesystem::create_symlink("missing-target", removal_symlink);
    try {
        static_cast<void>(safe_root.remove(
            removal_symlink,
            onedrive::sync::detail::FilesystemItemKind::file
        ));
        return fail("safe removal accepted a symbolic link");
    } catch (const detail::SafePathConflictError&) {
    }
    const auto local_source = root / "local.txt";
    {
        std::ofstream output{local_source, std::ios::binary};
        output << "local";
    }
    if (::chmod(local_source.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == -1) {
        return fail("safeBackup permission fixture could not be prepared");
    }
    const auto local_backup = detail::preserve_safe_backup(
        safe_root,
        local_source,
        detail::capture_local_file_baseline(local_source)
    );
    struct stat backup_status{};
    if (!local_backup.path.filename().string().starts_with(
            "local.safeBackup-"
        ) ||
        detail::content_fingerprint(local_backup.path) !=
            detail::content_fingerprint(local_source) ||
        ::stat(local_backup.path.c_str(), &backup_status) == -1 ||
        (backup_status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
            (S_IRUSR | S_IWUSR | S_IRGRP)) {
        return fail("safeBackup copy did not preserve content and permissions");
    }
    const auto stale_source = root / "stale.txt";
    {
        std::ofstream output{stale_source, std::ios::binary};
        output << "old";
    }
    const auto stale_baseline =
        detail::capture_local_file_baseline(stale_source);
    {
        std::ofstream output{stale_source, std::ios::binary};
        output << "changed";
    }
    try {
        static_cast<void>(detail::preserve_safe_backup(
            safe_root, stale_source, stale_baseline
        ));
        return fail("safeBackup accepted a stale local baseline");
    } catch (const detail::LocalModificationConflictError&) {
    }
    try {
        static_cast<void>(
            detail::preserve_safe_backup(safe_root, stale_source, {})
        );
        return fail("safeBackup accepted an absent local baseline");
    } catch (const std::invalid_argument&) {
    }
    const auto long_source = root / (std::string(230, 'x') + ".txt");
    {
        std::ofstream output{long_source, std::ios::binary};
        output << "long";
    }
    const auto long_backup = detail::preserve_safe_backup(
        safe_root, long_source, detail::capture_local_file_baseline(long_source)
    );
    if (!long_backup.path.filename().string().starts_with("onedrive-")) {
        return fail("long safeBackup name did not use a bounded digest");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_safe();
}
