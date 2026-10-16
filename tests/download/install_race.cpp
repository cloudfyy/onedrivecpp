#include "support.hpp"

#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <unistd.h>

namespace {

using namespace onedrive::test::download;
namespace detail = onedrive::sync::detail;
using Policy = onedrive::config::LocalConflictPolicy;

std::function<void(unsigned)> before_rename;
int rename_error = 0;
unsigned rejected_renames = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void inject(const char* destination, unsigned flags) {
    if (before_rename && std::string_view{destination} == "race.txt") {
        before_rename(flags);
    }
}

bool reject_rename(const char* destination) {
    if (rename_error != 0 && std::string_view{destination} == "race.txt") {
        ++rejected_renames;
        errno = rename_error;
        return true;
    }
    return false;
}

std::vector<std::filesystem::path> backups(const DownloadFixture& fixture) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry :
         std::filesystem::directory_iterator{fixture.root}) {
        if (entry.path().filename().string().contains(".safeBackup-")) {
            result.push_back(entry.path());
        }
    }
    return result;
}

struct RaceFixture {
    DownloadFixture fixture;
    const std::filesystem::path destination{fixture.root / "race.txt"};
    const onedrive::graph::RemoteItem item{remote_item("race", "race.txt")};
    onedrive::graph::GraphClient graph{
        onedrive::util::borrowed_proxy, fixture.graph
    };
    onedrive::storage::ItemStore items{
        onedrive::util::borrowed_proxy, fixture.items
    };

    ~RaceFixture() {
        before_rename = {};
        rename_error = 0;
        rejected_renames = 0;
    }

    void commit(Policy policy) {
        static_cast<void>(detail::commit_download(
            items,
            fixture.safe_root,
            fixture.metadata,
            detail::prepare_download(
                graph,
                items,
                item,
                item_state(item, destination),
                destination,
                detail::capture_local_file_baseline(destination),
                fixture.metadata,
                fixture.space,
                {}
            ),
            {.local_conflict = policy}
        ));
    }

    onedrive::storage::PendingDownload
    journal(bool existing, bool legacy = false) {
        if (existing) {
            onedrive::test::write_file(destination, "old");
        }
        const auto baseline = detail::capture_local_file_baseline(destination);
        const auto temporary = detail::temporary_path_for(destination);
        onedrive::test::write_file(temporary, "data");
        auto state = item_state(item, destination);
        state.local_size = baseline.size;
        state.local_modified_ticks = baseline.modified_ticks;
        if (existing) {
            fixture.items.upsert(state);
        }
        onedrive::storage::PendingDownload download{
            .item = state,
            .temporary_path = temporary,
            .content_fingerprint = detail::content_fingerprint(temporary),
            .replacement_fingerprint =
                !legacy && existing ? baseline.fingerprint : "",
        };
        fixture.items.save_pending_download(download);
        return download;
    }

    void recover(Policy policy) {
        detail::recover_pending_downloads(
            items,
            fixture.safe_root,
            "me",
            fixture.metadata,
            onedrive::config::SyncPermissionsMode::private_access,
            nullptr,
            policy
        );
    }
};

void check_edit_result(RaceFixture& test, Policy policy, bool conflicted) {
    require(
        conflicted == (policy == Policy::block),
        "late local edit did not follow the configured conflict policy"
    );
    if (policy == Policy::block) {
        require(
            onedrive::test::read_file(test.destination) == "USER-EDIT",
            "blocking installation overwrote the concurrent local edit"
        );
        require(
            test.fixture.items.pending.contains("race"),
            "blocked installation discarded its recovery journal"
        );
    } else {
        require(
            onedrive::test::read_file(test.destination) == "data" &&
                test.fixture.items.pending.empty(),
            "backup installation did not finish the remote download"
        );
        const auto saved = backups(test.fixture);
        require(
            saved.size() == 1 &&
                onedrive::test::read_file(saved.front()) == "USER-EDIT",
            "safeBackup did not retain the latest local edit"
        );
    }
}

void test_commit_edit(Policy policy, bool existing, bool atomic_save) {
    RaceFixture test;
    if (existing) {
        onedrive::test::write_file(test.destination, "old");
    }
    bool injected = false;
    before_rename = [&](unsigned) {
        if (!std::exchange(injected, true)) {
            if (atomic_save) {
                const auto edited = test.fixture.root / "edited.tmp";
                onedrive::test::write_file(edited, "USER-EDIT");
                std::filesystem::rename(edited, test.destination);
            } else {
                onedrive::test::write_file(test.destination, "USER-EDIT");
            }
        }
    };
    bool conflicted = false;
    try {
        test.commit(policy);
    } catch (const detail::LocalModificationConflictError&) {
        conflicted = true;
    }
    require(injected, "commit regression did not reach the install syscall");
    check_edit_result(test, policy, conflicted);
}

void test_path_type_change(Policy policy, bool symlink) {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    const auto outside = test.fixture.temporary.path() / "outside.txt";
    onedrive::test::write_file(outside, "outside");
    bool injected = false;
    before_rename = [&](unsigned) {
        if (!std::exchange(injected, true)) {
            std::filesystem::remove(test.destination);
            if (symlink) {
                std::filesystem::create_symlink(outside, test.destination);
            } else {
                std::filesystem::create_directory(test.destination);
                onedrive::test::write_file(
                    test.destination / "child.txt", "child"
                );
            }
        }
    };
    bool conflicted = false;
    try {
        test.commit(policy);
    } catch (const detail::LocalPathConflictError&) {
        conflicted = true;
    }
    require(conflicted, "installation accepted a late path-type conflict");
    const auto status = std::filesystem::symlink_status(test.destination);
    require(
        symlink ? std::filesystem::is_symlink(status)
                : std::filesystem::is_directory(status),
        "installation failed to restore the user's changed path type"
    );
    require(
        onedrive::test::read_file(outside) == "outside" &&
            (symlink ||
             onedrive::test::read_file(test.destination / "child.txt") ==
                 "child"),
        "path-type conflict modified user contents"
    );
}

void test_second_edit_during_rollback() {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    unsigned exchanges = 0;
    before_rename = [&](unsigned flags) {
        if (flags == RENAME_EXCHANGE) {
            onedrive::test::write_file(
                test.destination, ++exchanges == 1 ? "USER-EDIT" : "SECOND-EDIT"
            );
        }
    };
    bool conflicted = false;
    try {
        test.commit(Policy::block);
    } catch (const detail::LocalModificationConflictError&) {
        conflicted = true;
    }
    require(conflicted && exchanges == 2, "rollback race was not exercised");
    const auto pending = test.fixture.items.pending.at("race");
    require(
        onedrive::test::read_file(test.destination) == "USER-EDIT" &&
            onedrive::test::read_file(pending.temporary_path) == "SECOND-EDIT",
        "rollback overwrote one of the concurrent local versions"
    );
    before_rename = {};
    bool refused = false;
    try {
        test.recover(Policy::block);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(
        refused &&
            onedrive::test::read_file(pending.temporary_path) == "SECOND-EDIT",
        "recovery discarded an unknown displaced local version"
    );
}

void test_recovery_edit(Policy policy, bool existing, bool legacy) {
    RaceFixture test;
    static_cast<void>(test.journal(existing, legacy));
    bool injected = false;
    before_rename = [&](unsigned) {
        if (!std::exchange(injected, true)) {
            onedrive::test::write_file(test.destination, "USER-EDIT");
        }
    };
    bool conflicted = false;
    try {
        test.recover(policy);
    } catch (const detail::LocalModificationConflictError&) {
        conflicted = true;
    }
    require(injected, "recovery regression did not reach the install syscall");
    check_edit_result(test, policy, conflicted);
}

void test_exchange_recovery(Policy policy, bool exchanged, bool edited) {
    RaceFixture test;
    const auto pending = test.journal(true);
    if (exchanged) {
        test.fixture.safe_root.exchange(
            pending.temporary_path, test.destination
        );
    }
    if (edited) {
        onedrive::test::write_file(
            exchanged ? pending.temporary_path : test.destination, "USER-EDIT"
        );
    }
    bool conflicted = false;
    try {
        test.recover(policy);
    } catch (const detail::LocalModificationConflictError&) {
        conflicted = true;
    }
    if (edited) {
        check_edit_result(test, policy, conflicted);
    } else {
        require(
            !conflicted &&
                onedrive::test::read_file(test.destination) == "data" &&
                !std::filesystem::exists(pending.temporary_path) &&
                test.fixture.items.pending.empty() &&
                backups(test.fixture).empty(),
            "exchange recovery did not finish without unnecessary backups"
        );
    }
}

void test_missing_backup_recovery() {
    RaceFixture test;
    auto pending = test.journal(true);
    pending.backup_path = test.fixture.root / "missing.safeBackup.txt";
    pending.backup_fingerprint = pending.replacement_fingerprint;
    test.fixture.items.save_pending_download(pending);
    test.fixture.safe_root.exchange(pending.temporary_path, test.destination);
    bool refused = false;
    try {
        test.recover(Policy::backup);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(
        refused && onedrive::test::read_file(test.destination) == "old" &&
            test.fixture.items.pending.contains("race"),
        "recovery discarded the original when its promised backup was missing"
    );
}

void test_post_install_persistence_failure() {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    test.fixture.items.fail_upsert = true;
    bool failed = false;
    try {
        test.commit(Policy::block);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(
        failed && test.fixture.items.pending.contains("race") &&
            onedrive::test::read_file(test.destination) == "data",
        "persistence failure lost an exchanged download's journal"
    );
    test.fixture.items.fail_upsert = false;
    test.recover(Policy::block);
    require(
        test.fixture.items.pending.empty() &&
            test.fixture.items.find("me", "race").has_value(),
        "installed exchange could not recover after persistence failure"
    );
}

void test_late_deletion(Policy policy) {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    bool injected = false;
    before_rename = [&](unsigned) {
        if (!std::exchange(injected, true)) {
            std::filesystem::remove(test.destination);
        }
    };
    bool conflicted = false;
    try {
        test.commit(policy);
    } catch (const detail::LocalModificationConflictError&) {
        conflicted = true;
    }
    require(
        injected && conflicted && !std::filesystem::exists(test.destination) &&
            test.fixture.items.pending.contains("race"),
        "installation recreated a file deleted at the final syscall"
    );
}

void test_unsupported_atomic_operation(bool existing) {
    RaceFixture test;
    if (existing) {
        onedrive::test::write_file(test.destination, "old");
    }
    rename_error = EOPNOTSUPP;
    bool refused = false;
    try {
        test.commit(Policy::block);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(
        refused && rejected_renames == 1 &&
            test.fixture.items.pending.contains("race") &&
            (existing ? onedrive::test::read_file(test.destination) == "old"
                      : !std::filesystem::exists(test.destination)),
        "unsupported atomic installation used an unsafe fallback"
    );
    rename_error = 0;
    test.recover(Policy::block);
    require(
        onedrive::test::read_file(test.destination) == "data" &&
            test.fixture.items.pending.empty(),
        "unsupported atomic operation discarded its recoverable download"
    );
}

void test_backup_retains_open_file() {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    std::optional<onedrive::util::UniqueFD> source;
    before_rename = [&](unsigned) {
        if (!source) {
            onedrive::test::write_file(test.destination, "USER-EDIT");
            source.emplace(
                test.fixture.safe_root.open(test.destination, O_RDWR)
            );
            require(source->get() != -1, "cannot hold edited file open");
        }
    };
    bool updated_backup = false;
    test.fixture.items.pending_download_saved = [&] {
        if (source && !updated_backup &&
            !test.fixture.items.pending.at("race").backup_path.empty()) {
            constexpr std::string_view latest{"LATEST-FD-EDIT"};
            require(
                ::ftruncate(source->get(), 0) == 0 &&
                    ::pwrite(source->get(), latest.data(), latest.size(), 0) ==
                        static_cast<ssize_t>(latest.size()),
                "cannot update the still-open displaced file"
            );
            updated_backup = true;
        }
    };
    test.commit(Policy::backup);
    const auto saved = backups(test.fixture);
    require(
        updated_backup && saved.size() == 1 &&
            onedrive::test::read_file(saved.front()) == "LATEST-FD-EDIT",
        "safeBackup copied and discarded a still-open local version"
    );
}

void test_invalid_exchange_journal(bool invalid_path) {
    RaceFixture test;
    auto pending = test.journal(true);
    if (invalid_path) {
        pending.temporary_path = test.destination;
    } else {
        pending.replacement_fingerprint = "not-a-sha256";
    }
    test.fixture.items.save_pending_download(pending);
    bool refused = false;
    try {
        test.recover(Policy::backup);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(
        refused && onedrive::test::read_file(test.destination) == "old" &&
            test.fixture.items.pending.contains("race"),
        "malformed exchange journal modified local data"
    );
}

void test_wrong_journal_size(bool exchanged) {
    RaceFixture test;
    auto pending = test.journal(true);
    ++pending.item.size;
    test.fixture.items.save_pending_download(pending);
    if (exchanged) {
        test.fixture.safe_root.exchange(
            pending.temporary_path, test.destination
        );
    }
    bool refused = false;
    try {
        test.recover(Policy::block);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(
        refused &&
            onedrive::test::read_file(test.destination) ==
                (exchanged ? "data" : "old") &&
            std::filesystem::exists(pending.temporary_path) &&
            test.fixture.items.pending.contains("race"),
        "exchange recovery accepted content with the wrong journal size"
    );
}

} // namespace

extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __wrap_renameat(
    int source_directory,
    const char* source,
    int destination_directory,
    const char* destination
) {
    inject(destination, 0);
    if (reject_rename(destination)) {
        return -1;
    }
    return __real_renameat(
        source_directory, source, destination_directory, destination
    );
}

extern "C" int __real_renameat2(int, const char*, int, const char*, unsigned);
extern "C" int __wrap_renameat2(
    int source_directory,
    const char* source,
    int destination_directory,
    const char* destination,
    unsigned flags
) {
    inject(destination, flags);
    if (reject_rename(destination)) {
        return -1;
    }
    return __real_renameat2(
        source_directory, source, destination_directory, destination, flags
    );
}

int main() {
    try {
        for (const auto policy : {Policy::block, Policy::backup}) {
            for (const bool existing : {false, true}) {
                for (const bool atomic_save : {false, true}) {
                    test_commit_edit(policy, existing, atomic_save);
                }
                test_recovery_edit(policy, existing, false);
                for (const bool edited : {false, true}) {
                    test_exchange_recovery(policy, existing, edited);
                }
            }
            test_recovery_edit(policy, true, true);
            test_path_type_change(policy, false);
            test_path_type_change(policy, true);
            test_late_deletion(policy);
        }
        test_second_edit_during_rollback();
        test_missing_backup_recovery();
        test_post_install_persistence_failure();
        test_unsupported_atomic_operation(false);
        test_unsupported_atomic_operation(true);
        test_backup_retains_open_file();
        for (const bool state : {false, true}) {
            test_invalid_exchange_journal(state);
            test_wrong_journal_size(state);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
