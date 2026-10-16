#include "support.hpp"
#include "sync/download/installation.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <unistd.h>

namespace {

using namespace onedrive::test::download;
namespace detail = onedrive::sync::detail;
using Policy = onedrive::config::LocalConflictPolicy;

std::function<void(unsigned)> before_rename;
std::function<void()> before_sync;
std::function<int(std::string_view)> backup_rename;
int rename_error = 0;
unsigned rejected_renames = 0;
unsigned failing_sync = 0;

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
        before_sync = {};
        backup_rename = {};
        rename_error = 0;
        rejected_renames = 0;
        failing_sync = 0;
    }

    void commit(
        Policy policy,
        std::move_only_function<void(const std::filesystem::path&)>
            backup_created = {}
    ) {
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
            {
                .local_conflict = policy,
                .backup_created = std::move(backup_created),
            }
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

void test_installation_guards(bool same_path) {
    RaceFixture test;
    auto pending = test.journal(true);
    const auto temporary = pending.temporary_path;
    if (same_path) {
        pending.temporary_path = test.destination;
        require(
            onedrive::test::throws_with<std::invalid_argument>(
                [&] {
                    detail::install_pending_download(
                        test.items,
                        test.fixture.safe_root,
                        pending,
                        Policy::block
                    );
                },
                "must be distinct"
            ),
            "installation accepted overlapping source and destination"
        );
    } else {
        pending.item.local_path = test.destination / "child.txt";
        bool refused = false;
        try {
            detail::install_pending_download(
                test.items, test.fixture.safe_root, pending, Policy::block
            );
        } catch (const std::filesystem::filesystem_error& error) {
            refused = error.code() == std::errc::not_a_directory;
        }
        require(refused, "installation hid a destination inspection failure");
    }
    require(
        onedrive::test::read_file(test.destination) == "old" &&
            onedrive::test::read_file(temporary) == "data",
        "invalid installation paths modified file contents"
    );
}

void test_deleted_destination_recovery(Policy policy) {
    RaceFixture test;
    const auto pending = test.journal(true);
    std::filesystem::remove(test.destination);
    require(
        onedrive::test::throws_with<detail::LocalModificationConflictError>(
            [&] { test.recover(policy); }, "local file changed"
        ) && !std::filesystem::exists(test.destination) &&
            onedrive::test::read_file(pending.temporary_path) == "data" &&
            test.fixture.items.pending.contains("race"),
        "recovery recreated a deleted original or discarded its journal"
    );
}

void test_rollback_boundary(bool new_file, bool io_error) {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    unsigned exchanges = 0;
    before_rename = [&](unsigned flags) {
        if (flags == RENAME_EXCHANGE && ++exchanges == 1) {
            onedrive::test::write_file(test.destination, "USER-EDIT");
        } else if (flags == RENAME_EXCHANGE && exchanges == 2) {
            if (io_error) {
                rename_error = EIO;
            } else {
                std::filesystem::remove(test.destination);
            }
        } else if (flags == RENAME_NOREPLACE && new_file) {
            onedrive::test::write_file(test.destination, "SECOND-EDIT");
        }
    };
    require(
        onedrive::test::throws_with<std::runtime_error>(
            [&] { test.commit(Policy::block); },
            io_error ? "cannot atomically exchange" : "local file changed"
        ),
        "rollback failure did not propagate the expected error"
    );
    const auto pending = test.fixture.items.pending.at("race");
    require(
        onedrive::test::read_file(test.destination) ==
                (io_error ? "data" : "USER-EDIT") &&
            (io_error || new_file
                 ? onedrive::test::read_file(pending.temporary_path) ==
                       (io_error ? "USER-EDIT" : "SECOND-EDIT")
                 : !std::filesystem::exists(pending.temporary_path)),
        "rollback lost a local version at the missing-destination boundary"
    );
    before_rename = {};
    rename_error = 0;
    if (io_error) {
        require(
            onedrive::test::throws_with<detail::LocalModificationConflictError>(
                [&] { test.recover(Policy::block); }, "local file changed"
            ) && onedrive::test::read_file(test.destination) == "USER-EDIT",
            "recovery failed to restore the edit after a failed rollback"
        );
    }
}

void test_interrupted_exchange(unsigned sync_number, bool remove_displaced) {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    before_rename = [&](unsigned flags) {
        if (flags == RENAME_EXCHANGE) {
            if (remove_displaced) {
                before_sync = [&] {
                    if (std::filesystem::exists(
                            test.fixture.items.pending.at("race").temporary_path
                        )) {
                        std::filesystem::remove(
                            test.fixture.items.pending.at("race").temporary_path
                        );
                    }
                };
            } else {
                failing_sync = sync_number;
            }
        }
    };
    require(
        onedrive::test::throws_with<std::runtime_error>(
            [&] { test.commit(Policy::block); },
            remove_displaced ? "local file changed" : "cannot flush"
        ),
        "interrupted exchange was reported as committed"
    );
    const auto pending = test.fixture.items.pending.at("race");
    require(
        onedrive::test::read_file(test.destination) == "data" &&
            (remove_displaced || sync_number == 2
                 ? !std::filesystem::exists(pending.temporary_path)
                 : onedrive::test::read_file(pending.temporary_path) == "old"),
        "interrupted exchange left unexpected filesystem state"
    );
    before_rename = {};
    before_sync = {};
    failing_sync = 0;
    test.recover(Policy::block);
    require(
        test.fixture.items.pending.empty() &&
            onedrive::test::read_file(test.destination) == "data",
        "exchange interruption could not recover from its retained journal"
    );
}

void test_displaced_backup_collision(bool exhausted) {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    bool edited = false;
    before_rename = [&](unsigned flags) {
        if (flags == RENAME_EXCHANGE && !std::exchange(edited, true)) {
            onedrive::test::write_file(test.destination, "USER-EDIT");
        }
    };
    unsigned attempts = 0;
    std::filesystem::path collision;
    backup_rename = [&](std::string_view name) {
        ++attempts;
        if (exhausted) {
            return EEXIST;
        }
        if (attempts == 1) {
            collision = test.fixture.root / name;
            onedrive::test::write_file(collision, "EXISTING-BACKUP");
        }
        return 0;
    };
    if (exhausted) {
        require(
            onedrive::test::throws_with<std::runtime_error>(
                [&] { test.commit(Policy::backup); },
                "cannot allocate a unique displaced safeBackup"
            ) && attempts == 9999 &&
                onedrive::test::read_file(test.destination) == "USER-EDIT" &&
                onedrive::test::read_file(
                    test.fixture.items.pending.at("race").temporary_path
                ) == "data" &&
                backups(test.fixture).empty(),
            "backup-name exhaustion lost data or skipped the retry limit"
        );
    } else {
        std::filesystem::path published;
        test.commit(Policy::backup, [&](const auto& path) {
            published = path;
        });
        require(
            attempts == 2 && !published.empty() && published != collision &&
                onedrive::test::read_file(published) == "USER-EDIT" &&
                onedrive::test::read_file(collision) == "EXISTING-BACKUP" &&
                onedrive::test::read_file(test.destination) == "data",
            "backup-name collision overwrote an existing backup or hid "
            "notification"
        );
    }
}

void test_displaced_backup_disappears() {
    RaceFixture test;
    onedrive::test::write_file(test.destination, "old");
    bool edited = false;
    before_rename = [&](unsigned flags) {
        if (flags == RENAME_EXCHANGE && !std::exchange(edited, true)) {
            onedrive::test::write_file(test.destination, "USER-EDIT");
        }
    };
    std::filesystem::path published;
    backup_rename = [&](std::string_view name) {
        published = test.fixture.root / name;
        before_sync = [&] {
            if (std::filesystem::exists(published)) {
                std::filesystem::remove(published);
            }
        };
        return 0;
    };
    require(
        onedrive::test::throws_with<detail::LocalModificationConflictError>(
            [&] { test.commit(Policy::backup); },
            "displaced safeBackup disappeared"
        ) && !published.empty() &&
            !std::filesystem::exists(published) &&
            onedrive::test::read_file(test.destination) == "data" &&
            test.fixture.items.pending.contains("race"),
        "vanished displaced backup was silently accepted"
    );
}

void test_backup_flush_failure() {
    RaceFixture test;
    const auto pending = test.journal(true);
    onedrive::test::write_file(test.destination, "USER-EDIT");
    failing_sync = 1;
    require(
        onedrive::test::throws_with<std::runtime_error>(
            [&] { test.recover(Policy::backup); }, "cannot flush safeBackup"
        ) && onedrive::test::read_file(test.destination) == "USER-EDIT" &&
            onedrive::test::read_file(pending.temporary_path) == "data" &&
            test.fixture.items.pending.contains("race") &&
            backups(test.fixture).empty(),
        "failed backup flush discarded local data or left partial backup files"
    );
    failing_sync = 0;
    test.recover(Policy::backup);
    check_edit_result(test, Policy::backup, false);
}

void test_backup_without_callback() {
    RaceFixture test;
    auto pending = test.journal(true);
    test.fixture.safe_root.exchange(pending.temporary_path, test.destination);
    onedrive::test::write_file(pending.temporary_path, "USER-EDIT");
    detail::install_pending_download(
        test.items, test.fixture.safe_root, pending, Policy::backup
    );
    require(
        onedrive::test::read_file(pending.backup_path) == "USER-EDIT" &&
            !std::filesystem::exists(pending.temporary_path) &&
            test.fixture.items.pending.at("race").backup_path ==
                pending.backup_path,
        "installation without a callback failed to journal its local backup"
    );
    test.recover(Policy::backup);
    check_edit_result(test, Policy::backup, false);
}

class RecoveryObserver final : public onedrive::events::Observer {
public:
    mutable std::vector<onedrive::events::MessageEvent> messages;
    mutable std::vector<onedrive::events::FileOperationEvent> operations;

private:
    void on_event(const onedrive::events::Event& event) const override {
        if (const auto* message =
                std::get_if<onedrive::events::MessageEvent>(&event)) {
            messages.push_back(*message);
        }
        if (const auto* operation =
                std::get_if<onedrive::events::FileOperationEvent>(&event)) {
            operations.push_back(*operation);
        }
    }
};

void test_recovery_backup_notification() {
    RaceFixture test;
    const auto pending = test.journal(true);
    test.fixture.safe_root.exchange(pending.temporary_path, test.destination);
    onedrive::test::write_file(pending.temporary_path, "USER-EDIT");
    RecoveryObserver observer;
    detail::recover_pending_downloads(
        test.items,
        test.fixture.safe_root,
        "me",
        test.fixture.metadata,
        onedrive::config::SyncPermissionsMode::private_access,
        &observer,
        Policy::backup
    );
    const auto saved = backups(test.fixture);
    require(
        saved.size() == 1 &&
            std::ranges::any_of(
                observer.messages,
                [&](const auto& message) {
                    return message.kind ==
                               onedrive::events::MessageKind::warning &&
                           message.event == "local_conflict_backed_up" &&
                           message.text.contains(saved.front().string());
                }
            ) &&
            observer.operations.size() == 1 &&
            observer.operations[0].recovered &&
            observer.operations[0].action ==
                onedrive::events::FileAction::downloaded,
        "recovery did not publish the backup warning and recovered operation"
    );
    check_edit_result(test, Policy::backup, false);
}

void test_invalid_backup_journals() {
    RaceFixture test;
    auto pending = test.journal(true);
    const auto backup = test.fixture.root / "race.safeBackup.txt";
    const auto fingerprint = pending.replacement_fingerprint;
    const std::vector<std::pair<std::filesystem::path, std::string>> invalid{
        {{}, fingerprint},
        {backup, {}},
        {backup, std::string(64, 'g')},
        {backup, std::string(64, 'G')},
        {backup, std::string(64, '/')},
        {test.fixture.temporary.path() / "outside.txt", fingerprint},
        {test.destination, fingerprint},
        {pending.temporary_path, fingerprint},
    };
    for (const auto& [path, hash] : invalid) {
        pending.backup_path = path;
        pending.backup_fingerprint = hash;
        test.fixture.items.save_pending_download(pending);
        require(
            onedrive::test::throws_with<std::runtime_error>(
                [&] { test.recover(Policy::backup); },
                "invalid safeBackup metadata"
            ) && onedrive::test::read_file(test.destination) == "old" &&
                onedrive::test::read_file(pending.temporary_path) == "data" &&
                test.fixture.items.pending.contains("race"),
            "invalid backup journal modified or discarded local state"
        );
    }
}

void test_legacy_path_types(bool temporary_link, bool directory) {
    RaceFixture test;
    const auto pending = test.journal(true, true);
    const auto changed =
        temporary_link ? pending.temporary_path : test.destination;
    std::filesystem::remove(changed);
    const auto outside = test.fixture.temporary.path() / "outside.txt";
    onedrive::test::write_file(outside, "OUTSIDE");
    if (directory) {
        std::filesystem::create_directory(changed);
        onedrive::test::write_file(changed / "child", "CHILD");
    } else {
        std::filesystem::create_symlink(outside, changed);
    }
    require(
        onedrive::test::throws_with<std::runtime_error>(
            [&] { test.recover(Policy::backup); },
            directory ? "not a regular file" : "symbolic link"
        ) && onedrive::test::read_file(outside) == "OUTSIDE" &&
            (directory ? onedrive::test::read_file(changed / "child") == "CHILD"
                       : std::filesystem::is_symlink(
                             std::filesystem::symlink_status(changed)
                         )) &&
            test.fixture.items.pending.contains("race"),
        "legacy recovery followed or removed an unsupported local path"
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
    if (backup_rename &&
        std::string_view{destination}.contains(".safeBackup-")) {
        const int error = backup_rename(destination);
        if (error != 0) {
            errno = error;
            return -1;
        }
    }
    if (reject_rename(destination)) {
        return -1;
    }
    return __real_renameat2(
        source_directory, source, destination_directory, destination, flags
    );
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int descriptor) {
    if (before_sync) {
        before_sync();
    }
    if (failing_sync != 0 && --failing_sync == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
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
            test_deleted_destination_recovery(policy);
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
            test_installation_guards(state);
        }
        test_rollback_boundary(false, false);
        test_rollback_boundary(true, false);
        test_rollback_boundary(false, true);
        test_interrupted_exchange(1, false);
        test_interrupted_exchange(2, false);
        test_interrupted_exchange(0, true);
        test_displaced_backup_collision(false);
        test_displaced_backup_collision(true);
        test_displaced_backup_disappears();
        test_backup_flush_failure();
        test_backup_without_callback();
        test_recovery_backup_notification();
        test_invalid_backup_journals();
        test_legacy_path_types(false, false);
        test_legacy_path_types(true, false);
        test_legacy_path_types(false, true);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
