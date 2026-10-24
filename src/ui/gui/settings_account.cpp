#include "settings_editor.hpp"
#include <QFormLayout>
#include <QRegularExpression>
#include <QStringList>

namespace onedrive::gui {

void SettingsEditor::add_account_page() {
    auto* account = page();
    add(account,
        "auth",
        "application_id",
        QT_TR_NOOP("Application ID"),
        QT_TR_NOOP(
            "Your Entra public-client ID, not a secret; required for login "
            "(unset by default). Sign in again after changes."
        ),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.application_id); });
    add(account,
        "auth",
        "tenant_id",
        QT_TR_NOOP("Tenant ID"),
        QT_TR_NOOP(
            "Use common (default), consumers for personal accounts, or an "
            "organization's tenant ID matching your app registration."
        ),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.azure_tenant_id);
        });
    add(
        account,
        "sync",
        "data_directory",
        QT_TR_NOOP("Sync directory"),
        QT_TR_NOOP(
            "Root for separate account/drive folders (default: ~/OneDrive); "
            "must not overlap state or use symlinks. Changes do not move files."
        ),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.sync_data_directory.string());
        },
        {},
        true
    );
    add(
        account,
        "state",
        "directory",
        QT_TR_NOOP("State directory"),
        QT_TR_NOOP(
            "Private credentials and sync state (default: "
            "~/.local/state/onedrive-cpp). Changes do not migrate state; sign "
            "in again."
        ),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.state_directory.string());
        },
        {},
        true
    );
    add(account,
        "sync",
        "drive_ids",
        QT_TR_NOOP("Drive IDs (one per line)"),
        QT_TR_NOOP(
            "Use me for your default OneDrive or actual Drive IDs for "
            "libraries. "
            "All entries use the same account and synchronize sequentially."
        ),
        Kind::list,
        [](const auto& c) {
            QStringList ids;
            for (const auto& id : c.configured_drive_ids()) {
                ids.push_back(QString::fromStdString(id));
            }
            return ids.join('\n');
        });
    note(
        account,
        QT_TR_NOOP(
            "Directory changes do not migrate files, account state or "
            "credentials. Settings take effect on the next operation; running "
            "operations are not changed."
        )
    );
    auto* account_advanced =
        advanced(account, QT_TR_NOOP("Advanced account settings"));
    add(account_advanced,
        "auth",
        "endpoint",
        QT_TR_NOOP("Authentication endpoint"),
        QT_TR_NOOP(
            "Trusted HTTPS login URL: login.microsoftonline.com (default) or "
            "login.chinacloudapi.cn. Match the Graph cloud; sign in again "
            "after changes."
        ),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.auth_endpoint); });
    add(account_advanced,
        "graph",
        "endpoint",
        QT_TR_NOOP("Graph endpoint"),
        QT_TR_NOOP(
            "Trusted HTTPS API receiving tokens: graph.microsoft.com/v1.0 "
            "(default) or microsoftgraph.chinacloudapi.cn/v1.0. Match the "
            "login cloud, not a SharePoint site."
        ),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.graph_endpoint); });
    add(account_advanced,
        "auth",
        "scopes",
        QT_TR_NOOP("Authentication scopes (one per line)"),
        QT_TR_NOOP(
            "Delegated permissions (default: User.Read, Files.ReadWrite, "
            "offline_access); request only what you need. Changes require "
            "signing in again."
        ),
        Kind::list,
        [](const auto& c) {
            return QString::fromStdString(c.auth_scope)
                .split(QRegularExpression{"\\s+"}, Qt::SkipEmptyParts)
                .join('\n');
        });
    add(
        account_advanced,
        "sync",
        "data_mount_point",
        QT_TR_NOOP("Required mount point (empty: disabled)"),
        QT_TR_NOOP(
            "Require a mounted ancestor before sync to prevent missing-storage "
            "deletions; empty disables this guard (default)."
        ),
        Kind::optional_path,
        [](const auto& c) {
            return c.sync_data_mount_point
                       ? QString::fromStdString(
                             c.sync_data_mount_point->string()
                         )
                       : QString{};
        },
        {},
        true
    );
    note(
        account_advanced,
        QT_TR_NOOP(
            "Changing authentication endpoints or scopes requires signing in "
            "again. "
            "Use only trusted cloud endpoints; tokens may be sent to the Graph "
            "endpoint. "
            "A mount point is a preflight requirement, not a directory "
            "migration."
        )
    );
}

void SettingsEditor::add_sync_page() {
    auto* sync = page();
    add(sync,
        "sync",
        "mode",
        QT_TR_NOOP("Sync mode"),
        QT_TR_NOOP(
            "Choose which direction changes travel (default: both); deletion "
            "propagation is controlled separately."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.sync_mode) {
            case onedrive::sync::SyncMode::upload_only:
                return QString{"upload_only"};
            case onedrive::sync::SyncMode::download_only:
                return QString{"download_only"};
            default:
                return QString{"bidirectional"};
            }
        },
        {{"bidirectional", QT_TR_NOOP("Bidirectional")},
         {"upload_only", QT_TR_NOOP("Upload only")},
         {"download_only", QT_TR_NOOP("Download only")}});
    add(sync,
        "sync",
        "delete_policy",
        QT_TR_NOOP("Deletion policy"),
        QT_TR_NOOP(
            "Propagate deletes tracked destination items; preserve keeps "
            "them—inspect a dry run first. Default: propagate (upload-only: "
            "preserve unless set)."
        ),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.delete_policy == onedrive::sync::DeletePolicy::preserve
                    ? "preserve"
                    : "propagate"
            };
        },
        {{"propagate", QT_TR_NOOP("Propagate deletions")},
         {"preserve", QT_TR_NOOP("Preserve files")}});
    add(sync,
        "sync",
        "local_conflict",
        QT_TR_NOOP("Local conflict policy"),
        QT_TR_NOOP(
            "Block (default) stops conflicting changes; backup saves a local "
            "safeBackup copy before replacing with remote content and needs "
            "extra space."
        ),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.local_conflict == config::LocalConflictPolicy::block
                    ? "block"
                    : "backup"
            };
        },
        {{"block", QT_TR_NOOP("Block")},
         {"backup", QT_TR_NOOP("Back up conflicts")}});
    add(sync,
        "sync",
        "sync_list",
        QT_TR_NOOP("Selective sync rules file (empty: disabled)"),
        QT_TR_NOOP(
            "UTF-8 rules path relative to the config; unset selects all, an "
            "empty rules file selects nothing. Excluded local files are "
            "retained."
        ),
        Kind::optional_path,
        [](const auto& c) {
            return c.sync_list ? QString::fromStdString(c.sync_list->string())
                               : QString{};
        });
    add(sync,
        "sync",
        "dotfiles",
        QT_TR_NOOP("Dotfiles"),
        QT_TR_NOOP(
            "Include (default) syncs dot-prefixed paths; exclude skips them "
            "without deleting existing local copies."
        ),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.dotfiles == config::DotfilePolicy::include ? "include"
                                                             : "exclude"
            };
        },
        {{"include", QT_TR_NOOP("Include")},
         {"exclude", QT_TR_NOOP("Exclude")}});
    add(sync,
        "sync",
        "maximum_file_size_bytes",
        QT_TR_NOOP("Maximum file size (bytes; 0: unlimited)"),
        QT_TR_NOOP(
            "Skip larger files in both sync directions, retaining existing "
            "local copies; default: 0 (unlimited)."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_file_size_bytes);
        });
    auto* sync_advanced = advanced(sync, QT_TR_NOOP("Advanced sync rules"));
    add(sync_advanced,
        "sync",
        "nosync_enabled",
        QT_TR_NOOP("Honor .nosync files"),
        QT_TR_NOOP(
            "Enabled by default: a regular .nosync file excludes its local "
            "subtree; symlinks are not markers and the marker is never synced."
        ),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.nosync_enabled ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "sync_root_files",
        QT_TR_NOOP("Include root files with selective sync"),
        QT_TR_NOOP(
            "With selective rules, include root-level files, not directories; "
            "exclusions still win. Default: off; no effect without a rules "
            "file."
        ),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.sync_root_files ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "maximum_remote_deletions",
        QT_TR_NOOP("Maximum remote deletions (0: no deletions)"),
        QT_TR_NOOP(
            "Stop deletion batches exceeding this tracked-item count, "
            "including directory descendants; default: 1000, 0 blocks all "
            "remote deletions."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_remote_deletions);
        });
}

} // namespace onedrive::gui
