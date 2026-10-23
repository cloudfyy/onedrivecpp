#include "settings_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <stdexcept>
#include <algorithm>

namespace onedrive::gui {

SettingsEditor::SettingsEditor(QWidget* parent)
    : QWidget{parent} {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget{this};
    tabs_->setObjectName("settingsTabs");
    layout->addWidget(tabs_);
    const auto page = [this] {
        auto* scroll = new QScrollArea{tabs_};
        scroll->setWidgetResizable(true);
        auto* contents = new QWidget{scroll};
        auto* form = new QFormLayout{contents};
        scroll->setWidget(contents);
        tabs_->addTab(scroll, {});
        return form;
    };
    const auto note = [this](QFormLayout* form, const char* text) {
        auto* label = new QLabel;
        label->setWordWrap(true);
        form->addRow(label);
        notes_.emplace_back(label, text);
    };
    const auto advanced = [this](QFormLayout* form, const char* title) {
        auto* toggle = new QToolButton;
        toggle->setCheckable(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        auto* contents = new QWidget;
        auto* advanced_form = new QFormLayout{contents};
        form->addRow(toggle);
        form->addRow(contents);
        contents->hide();
        connect(
            toggle,
            &QToolButton::toggled,
            contents,
            [contents, toggle](bool expanded) {
                contents->setVisible(expanded);
                toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            }
        );
        advanced_.emplace_back(toggle, title);
        return advanced_form;
    };
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

    auto* transfer = page();
    add(transfer,
        "upload",
        "concurrency",
        QT_TR_NOOP("Upload concurrency (1–16)"),
        QT_TR_NOOP(
            "Independent files uploading at once (default: 1); each file's "
            "session fragments stay sequential."
        ),
        Kind::number,
        [](const auto& c) { return QString::number(c.upload_concurrency); });
    add(transfer,
        "download",
        "concurrency",
        QT_TR_NOOP("Download concurrency (1–16)"),
        QT_TR_NOOP(
            "Files downloading at once (default: 4); the same destination is "
            "serialized and workers share any total rate limit."
        ),
        Kind::number,
        [](const auto& c) { return QString::number(c.download_concurrency); });
    add(transfer,
        "upload",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Upload rate per request (bytes/s; 0: unlimited)"),
        QT_TR_NOOP(
            "Each upload request's speed, not the combined speed; default: 0 "
            "(unlimited). Low limits may require longer timeouts."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "download",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Download rate per request (bytes/s; 0: unlimited)"),
        QT_TR_NOOP(
            "Each file-content request's speed, not the combined speed; "
            "default: 0 (unlimited). Low limits may require longer timeouts."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "upload",
        "maximum_total_rate_bytes_per_second",
        QT_TR_NOOP("Aggregate upload rate (bytes/s; 0: unlimited)"),
        QT_TR_NOOP(
            "Combined speed shared by all active uploads (default: 0, "
            "unlimited); individual request limits still apply."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.upload_maximum_total_rate_bytes_per_second
            );
        });
    add(transfer,
        "download",
        "maximum_total_rate_bytes_per_second",
        QT_TR_NOOP("Aggregate download rate (bytes/s; 0: unlimited)"),
        QT_TR_NOOP(
            "Combined speed shared by all active downloads (default: 0, "
            "unlimited); individual request limits still apply."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.download_maximum_total_rate_bytes_per_second
            );
        });
    add(transfer,
        "transfer",
        "order",
        QT_TR_NOOP("Transfer order"),
        QT_TR_NOOP(
            "Download start order, not completion order; default preserves "
            "plan order. Upload dependencies remain parent-first."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.transfer_order) {
            case config::TransferOrder::size_ascending:
                return QString{"size_asc"};
            case config::TransferOrder::size_descending:
                return QString{"size_dsc"};
            case config::TransferOrder::name_ascending:
                return QString{"name_asc"};
            case config::TransferOrder::name_descending:
                return QString{"name_dsc"};
            default:
                return QString{"default"};
            }
        },
        {{"default", QT_TR_NOOP("Default")},
         {"size_asc", QT_TR_NOOP("Smallest first")},
         {"size_dsc", QT_TR_NOOP("Largest first")},
         {"name_asc", QT_TR_NOOP("Name ascending")},
         {"name_dsc", QT_TR_NOOP("Name descending")}});
    auto* transfer_advanced =
        advanced(transfer, QT_TR_NOOP("Advanced transfer settings"));
    add(transfer_advanced,
        "upload",
        "chunk_size_bytes",
        QT_TR_NOOP(
            "Upload chunk (bytes; multiple of 327680, less than 62914560)"
        ),
        QT_TR_NOOP(
            "Session fragment size (default: 10485760 bytes, 10 MiB); only "
            "files larger than 250000000 bytes use sessions."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_chunk_size_bytes);
        });
    add(transfer_advanced,
        "download",
        "chunk_threshold_bytes",
        QT_TR_NOOP("Download chunk threshold (bytes; greater than 0)"),
        QT_TR_NOOP(
            "Larger files use sequential ranges of at most this size (default: "
            "8388608 bytes, 8 MiB); relaxed validation disables chunking."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_chunk_threshold_bytes);
        });
    add(transfer_advanced,
        "download",
        "checkpoint_interval_bytes",
        QT_TR_NOOP("Download checkpoint interval (bytes; greater than 0)"),
        QT_TR_NOOP(
            "Persist progress after this many new bytes (default: 1048576, 1 "
            "MiB); smaller intervals reduce re-download but increase "
            "disk/database work."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_checkpoint_interval_bytes);
        });
    add(transfer_advanced,
        "download",
        "maximum_retries",
        QT_TR_NOOP("Maximum download retries"),
        QT_TR_NOOP(
            "Transient file-content retries (default: 4, 0 disables); separate "
            "from Graph API retries but uses Graph backoff delays."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_retries);
        });
    add(transfer_advanced,
        "download",
        "validation",
        QT_TR_NOOP("Download validation"),
        QT_TR_NOOP(
            "Strict (default) checks remote size/hashes; relaxed accepts "
            "transformed content but disables those checks and resume/chunking "
            "for all downloads, weakening integrity."
        ),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.download_validation == config::DownloadValidationMode::strict
                    ? "strict"
                    : "relaxed"
            };
        },
        {{"strict", QT_TR_NOOP("Strict")}, {"relaxed", QT_TR_NOOP("Relaxed")}});

    auto* network = page();
    add(
        network,
        "proxy",
        "url",
        QT_TR_NOOP("Proxy URL (empty override: direct connection)"),
        QT_TR_NOOP(
            "Unchecked (default) inherits proxy environment variables; checked "
            "with an empty URL forces direct connections. Never embed "
            "passwords; socks5h uses proxy DNS."
        ),
        Kind::override_text,
        [](const auto& c) {
            return c.proxy.url ? QString::fromStdString(*c.proxy.url)
                               : QString{};
        },
        {},
        false,
        [](const auto& c) { return c.proxy.url.has_value(); }
    );
    add(
        network,
        "proxy",
        "no_proxy",
        QT_TR_NOOP("Proxy bypass hosts (one per line; empty override: none)"),
        QT_TR_NOOP(
            "Unchecked (default) inherits NO_PROXY/no_proxy independently; "
            "checked with an empty list sends all hosts through the selected "
            "proxy."
        ),
        Kind::override_list,
        [](const auto& c) {
            QStringList entries;
            if (c.proxy.no_proxy) {
                for (const auto& entry : *c.proxy.no_proxy) {
                    entries.push_back(QString::fromStdString(entry));
                }
            }
            return entries.join('\n');
        },
        {},
        false,
        [](const auto& c) { return c.proxy.no_proxy.has_value(); }
    );
    add(network,
        "proxy",
        "username",
        QT_TR_NOOP("Proxy username (empty: unset)"),
        QT_TR_NOOP(
            "Optional proxy login name (default: unset); required with a "
            "password file. Clear credentials to return to environment-only "
            "settings."
        ),
        Kind::optional_text,
        [](const auto& c) {
            return c.proxy.username ? QString::fromStdString(*c.proxy.username)
                                    : QString{};
        });
    add(network,
        "proxy",
        "password_file",
        QT_TR_NOOP("Proxy password file (empty: unset)"),
        QT_TR_NOOP(
            "Optional path relative to the config (default: unset); requires a "
            "username and an owner-only regular file, no symlinks, at most 64 "
            "KiB."
        ),
        Kind::optional_path,
        [](const auto& c) {
            return c.proxy.password_file
                       ? QString::fromStdString(c.proxy.password_file->string())
                       : QString{};
        });
    add(network,
        "proxy",
        "auth",
        QT_TR_NOOP("Proxy authentication"),
        QT_TR_NOOP(
            "HTTP/HTTPS proxy authentication (default: auto); availability "
            "depends on libcurl. SOCKS5 uses its own credential mechanism."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.proxy.auth) {
            case http::ProxyAuth::basic:
                return QString{"basic"};
            case http::ProxyAuth::digest:
                return QString{"digest"};
            case http::ProxyAuth::ntlm:
                return QString{"ntlm"};
            case http::ProxyAuth::negotiate:
                return QString{"negotiate"};
            default:
                return QString{"auto"};
            }
        },
        {{"auto", QT_TR_NOOP("Automatic")},
         {"basic", QT_TR_NOOP("Basic")},
         {"digest", QT_TR_NOOP("Digest")},
         {"ntlm", QT_TR_NOOP("NTLM")},
         {"negotiate", QT_TR_NOOP("Negotiate")}});
    add(network,
        "proxy",
        "ca_file",
        QT_TR_NOOP("HTTPS proxy CA file (empty: unset)"),
        QT_TR_NOOP(
            "CA path relative to the config for an explicit HTTPS proxy, not "
            "the destination server; empty uses system trust and verification "
            "stays enabled."
        ),
        Kind::optional_path,
        [](const auto& c) {
            return c.proxy.ca_file
                       ? QString::fromStdString(c.proxy.ca_file->string())
                       : QString{};
        });
    note(
        network,
        QT_TR_NOOP(
            "Unchecked overrides inherit proxy environment variables "
            "independently. "
            "To inherit again, "
            "uncheck both overrides and clear credentials and CA settings. "
            "Never put passwords in the URL: use a private password file owned "
            "by you. "
            "Files are referenced, not copied or read by this editor; runtime "
            "security "
            "checks still apply. A password file requires a username, and a CA "
            "file "
            "requires an HTTPS proxy."
        )
    );
    add(network,
        "transfer",
        "connect_timeout_seconds",
        QT_TR_NOOP("Connection timeout (seconds; greater than 0)"),
        QT_TR_NOOP(
            "Connection time allowed per upload/download request (default: 30 "
            "seconds), separate from WebSocket timeouts."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.transfer_transport.connect_timeout.count()
            );
        });
    add(network,
        "transfer",
        "operation_timeout_seconds",
        QT_TR_NOOP("Request timeout (seconds; greater than 0)"),
        QT_TR_NOOP(
            "Total time per file-content request (default: 3600 seconds), "
            "including rate-limit waits; low limits may need more time."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.transfer_transport.operation_timeout.count()
            );
        });
    add(network,
        "transfer",
        "stall_timeout_seconds",
        QT_TR_NOOP("Stall timeout (seconds; 0: disabled)"),
        QT_TR_NOOP(
            "Abort transfers below the minimum rate for this long (default: 60 "
            "seconds); 0 disables detection and low rate limits can trigger it."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.transfer_transport.low_speed_timeout.count()
            );
        });
    add(network,
        "transfer",
        "stall_minimum_bytes_per_second",
        QT_TR_NOOP("Stall minimum rate (bytes/s; greater than 0)"),
        QT_TR_NOOP(
            "Speed below which the stall timer runs (default: 1 byte/second); "
            "no effect when stall timeout is 0."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.transfer_transport.low_speed_limit_bytes_per_second
            );
        });
    add(network,
        "transfer",
        "http_version",
        QT_TR_NOOP("HTTP version"),
        QT_TR_NOOP(
            "File-transfer protocol preference (default: auto); HTTP/2 may "
            "fall back depending on libcurl and server support."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.transfer_transport.http_version) {
            case http::HttpVersion::http_1_1:
                return QString{"1.1"};
            case http::HttpVersion::http_2:
                return QString{"2"};
            default:
                return QString{"auto"};
            }
        },
        {{"auto", QT_TR_NOOP("Automatic")},
         {"1.1", QT_TR_NOOP("HTTP/1.1")},
         {"2", QT_TR_NOOP("HTTP/2")}});
    add(network,
        "transfer",
        "ip_version",
        QT_TR_NOOP("IP version"),
        QT_TR_NOOP(
            "File-transfer address family (default: auto); forcing IPv4/IPv6 "
            "fails if the host has no matching address."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.transfer_transport.ip_version) {
            case http::IpVersion::ipv4:
                return QString{"4"};
            case http::IpVersion::ipv6:
                return QString{"6"};
            default:
                return QString{"auto"};
            }
        },
        {{"auto", QT_TR_NOOP("Automatic")},
         {"4", QT_TR_NOOP("IPv4")},
         {"6", QT_TR_NOOP("IPv6")}});
    auto* network_advanced = advanced(network, QT_TR_NOOP("Graph throttling"));
    add(network_advanced,
        "graph.throttle",
        "maximum_retries",
        QT_TR_NOOP("Maximum throttle retries (0: no retries)"),
        QT_TR_NOOP(
            "Graph API throttling/transient retries (default: 4, 0 disables); "
            "separate from file-content download retries."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_maximum_throttle_retries);
        });
    add(network_advanced,
        "graph.throttle",
        "initial_delay_seconds",
        QT_TR_NOOP("Initial throttle delay (seconds)"),
        QT_TR_NOOP(
            "Initial exponential retry delay (default: 1 second), also used by "
            "downloads; server Retry-After may override it."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_initial_throttle_delay.count());
        });
    add(network_advanced,
        "graph.throttle",
        "maximum_delay_seconds",
        QT_TR_NOOP("Maximum throttle delay (seconds; at least initial delay)"),
        QT_TR_NOOP(
            "Maximum retry wait (default: 300 seconds), at least the initial "
            "delay; a longer server Retry-After stops retries rather than "
            "shortening its wait."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_maximum_throttle_delay.count());
        });

    auto* monitor = page();
    add(monitor,
        "monitor",
        "websocket_enabled",
        QT_TR_NOOP("Enable WebSocket notifications"),
        QT_TR_NOOP(
            "Wake the CLI/service monitor on remote changes (default: on); "
            "disabling keeps local events and polling. The GUI does not run "
            "monitors."
        ),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.monitor_websocket_enabled ? "true" : "false"};
        });
    add(monitor,
        "monitor",
        "poll_interval_seconds",
        QT_TR_NOOP("Poll interval (seconds; 1–86400)"),
        QT_TR_NOOP(
            "Monitor's remote-change fallback interval (default: 300 seconds); "
            "polling continues without WebSocket notifications."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_poll_interval.count());
        });
    add(monitor,
        "monitor",
        "settle_delay_milliseconds",
        QT_TR_NOOP("Local settle delay (milliseconds; 0–60000)"),
        QT_TR_NOOP(
            "Wait for local-change bursts to settle (default: 1000 "
            "milliseconds); new events reset the delay and 0 skips waiting."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_settle_delay.count());
        });
    auto* monitor_advanced =
        advanced(monitor, QT_TR_NOOP("Advanced WebSocket settings"));
    add(monitor_advanced,
        "monitor",
        "websocket_request_timeout_seconds",
        QT_TR_NOOP("WebSocket request timeout (seconds; 1–300)"),
        QT_TR_NOOP(
            "Time allowed to acquire a Graph notification channel (default: 60 "
            "seconds), separate from file-transfer timeouts."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_request_timeout.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_connect_timeout_seconds",
        QT_TR_NOOP("WebSocket connection timeout (seconds; 1–300)"),
        QT_TR_NOOP(
            "Time allowed for the WSS connection/handshake (default: 10 "
            "seconds), separate from file-transfer timeouts."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_connect_timeout.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_renewal_lead_seconds",
        QT_TR_NOOP("WebSocket renewal lead (seconds; 0–3600)"),
        QT_TR_NOOP(
            "Refresh the channel before expiry (default: 120 seconds early); 0 "
            "renews at expiry rather than disabling renewal."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_renewal_lead.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_initial_backoff_seconds",
        QT_TR_NOOP("WebSocket initial backoff (seconds; 1–3600)"),
        QT_TR_NOOP(
            "Initial exponential reconnect delay (default: 1 second); polling "
            "provides fallback while disconnected."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_initial_backoff.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_maximum_backoff_seconds",
        QT_TR_NOOP(
            "WebSocket maximum backoff (seconds; 1–86400; at least initial "
            "backoff)"
        ),
        QT_TR_NOOP(
            "Reconnect delay ceiling (default: 300 seconds), at least the "
            "initial backoff; does not change server heartbeat timing."
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_maximum_backoff.count());
        });
    note(
        monitor,
        QT_TR_NOOP(
            "These settings configure the next CLI or service monitor "
            "operation. "
            "This GUI does not yet start synchronization or monitoring workers."
        )
    );

    auto* diagnostics = page();
    add(diagnostics,
        "history",
        "maximum_entries",
        QT_TR_NOOP("File history limit"),
        QT_TR_NOOP(
            "Completed operations retained since startup (default: 10000; "
            "greater than 0). Oldest entries are omitted when full. Shared by "
            "CLI and GUI; history is not saved after exit."
        ),
        Kind::number,
        [](const auto& c) { return QString::number(c.file_history_limit); });
    add(diagnostics,
        "logging",
        "level",
        QT_TR_NOOP("Logging level"),
        QT_TR_NOOP(
            "Diagnostic verbosity (default: info); debug/trace may expose "
            "private metadata, so review before sharing."
        ),
        Kind::choice,
        [](const auto& c) { return QString::fromStdString(c.logging.level); },
        {{"trace", QT_TR_NOOP("Trace")},
         {"debug", QT_TR_NOOP("Debug")},
         {"info", QT_TR_NOOP("Info")},
         {"warn", QT_TR_NOOP("Warning")},
         {"error", QT_TR_NOOP("Error")},
         {"critical", QT_TR_NOOP("Critical")},
         {"off", QT_TR_NOOP("Off")}});
    add(diagnostics,
        "logging",
        "file",
        QT_TR_NOOP("Log file (empty: unset)"),
        QT_TR_NOOP(
            "Optional log path relative to the config; empty disables file "
            "logging (default). Review private metadata before sharing."
        ),
        Kind::optional_path,
        [](const auto& c) {
            return c.logging.file
                       ? QString::fromStdString(c.logging.file->string())
                       : QString{};
        });
    add(diagnostics,
        "sync",
        "permissions",
        QT_TR_NOOP("Sync file permissions"),
        QT_TR_NOOP(
            "Private (default): new files 0600, root/new directories 0700; "
            "umask may expose content to others. Existing files are unchanged; "
            "state stays private."
        ),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.sync_permissions == config::SyncPermissionsMode::umask
                    ? "umask"
                    : "private"
            };
        },
        {{"private", QT_TR_NOOP("Private (owner only)")},
         {"umask", QT_TR_NOOP("Use process umask")}});
    add(diagnostics,
        "filesystem",
        "metadata",
        QT_TR_NOOP("Filesystem metadata"),
        QT_TR_NOOP(
            "Sync metadata always uses SQLite; auto (default) adds xattr hints "
            "when supported, xattr requires support, database uses no xattrs."
        ),
        Kind::choice,
        [](const auto& c) {
            switch (c.filesystem_metadata) {
            case config::FilesystemMetadataMode::xattr:
                return QString{"xattr"};
            case config::FilesystemMetadataMode::database:
                return QString{"database"};
            default:
                return QString{"auto"};
            }
        },
        {{"auto", QT_TR_NOOP("Automatic")},
         {"xattr", QT_TR_NOOP("Extended attributes")},
         {"database", QT_TR_NOOP("Database")}});
    note(
        diagnostics,
        QT_TR_NOOP(
            "The active configuration path is shown above. For diagnostics, "
            "use "
            "the CLI doctor command and check the configured log file. Debug "
            "and trace "
            "logs may contain private metadata; review them before sharing. "
            "Permission changes affect newly created files, not existing files."
        )
    );
    retranslate();
}

void SettingsEditor::add(
    QFormLayout* form,
    const char* table,
    const char* key,
    const char* label,
    const char* tip,
    Kind kind,
    std::function<QString(const config::Config&)> read,
    std::vector<std::pair<const char*, const char*>> choices,
    bool directory,
    std::function<bool(const config::Config&)> present
) {
    QWidget* widget;
    if (kind == Kind::choice) {
        auto* combo = new QComboBox;
        for (const auto& choice : choices) {
            combo->addItem(QString{}, QString::fromUtf8(choice.first));
        }
        widget = combo;
    } else if (kind == Kind::boolean) {
        widget = new QCheckBox;
    } else if (kind == Kind::list || kind == Kind::override_list) {
        auto* edit = new QPlainTextEdit;
        edit->setMaximumHeight(90);
        widget = edit;
    } else {
        auto* edit = new QLineEdit;
        if (kind == Kind::number) {
            edit->setPlaceholderText("0–9223372036854775807");
        }
        widget = edit;
    }
    widget->setObjectName(QString::fromUtf8(table) + "." + key);
    auto* label_widget = new QLabel;
    label_widget->setBuddy(widget);
    QWidget* container = widget;
    if (directory || kind == Kind::optional_path) {
        container = new QWidget;
        auto* layout = new QHBoxLayout{container};
        layout->setContentsMargins(0, 0, 0, 0);
        auto* browse = new QPushButton{tr("Browse…")};
        browse->setObjectName("browseButton");
        layout->addWidget(widget);
        layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, widget, directory] {
            auto* edit = static_cast<QLineEdit*>(widget);
            const auto selected =
                directory ? QFileDialog::getExistingDirectory(
                                this, tr("Select directory"), edit->text()
                            )
                          : QFileDialog::getOpenFileName(
                                this, tr("Select file"), edit->text()
                            );
            if (!selected.isEmpty()) {
                edit->setText(selected);
            }
        });
    }
    QCheckBox* override = nullptr;
    if (kind == Kind::override_text || kind == Kind::override_list) {
        container = new QWidget;
        auto* layout = new QVBoxLayout{container};
        layout->setContentsMargins(0, 0, 0, 0);
        override = new QCheckBox{tr("Override environment")};
        override->setObjectName(
            QString::fromUtf8(table) + "." + key + ".override"
        );
        layout->addWidget(override);
        layout->addWidget(widget);
        connect(override, &QCheckBox::toggled, widget, &QWidget::setEnabled);
    }
    form->addRow(label_widget, container);
    fields_.push_back(
        {table,
         key,
         label,
         tip,
         kind,
         widget,
         label_widget,
         container,
         std::move(read),
         {},
         std::move(choices),
         override,
         std::move(present),
         false}
    );
}

QString SettingsEditor::value(const Field& field) {
    if (field.kind == Kind::choice) {
        return static_cast<QComboBox*>(field.widget)->currentData().toString();
    }
    if (field.kind == Kind::boolean) {
        return static_cast<QCheckBox*>(field.widget)->isChecked() ? "true"
                                                                  : "false";
    }
    if (field.kind == Kind::list || field.kind == Kind::override_list) {
        return static_cast<QPlainTextEdit*>(field.widget)->toPlainText();
    }
    return static_cast<QLineEdit*>(field.widget)->text();
}

void SettingsEditor::load(const config::Config& config) {
    for (auto& field : fields_) {
        field.baseline = field.read(config);
        if (field.override) {
            field.baseline_present = field.present(config);
            field.override->setChecked(field.baseline_present);
            field.widget->setEnabled(field.baseline_present);
        }
        if (field.kind == Kind::choice) {
            auto* combo = static_cast<QComboBox*>(field.widget);
            combo->setCurrentIndex(combo->findData(field.baseline));
        } else if (field.kind == Kind::boolean) {
            static_cast<QCheckBox*>(field.widget)
                ->setChecked(field.baseline == "true");
        } else if (field.kind == Kind::list ||
                   field.kind == Kind::override_list) {
            static_cast<QPlainTextEdit*>(field.widget)
                ->setPlainText(field.baseline);
        } else {
            static_cast<QLineEdit*>(field.widget)->setText(field.baseline);
        }
    }
}

void SettingsEditor::retranslate() {
    tabs_->setTabText(0, tr("Account and directories"));
    tabs_->setTabText(1, tr("Sync rules"));
    tabs_->setTabText(2, tr("Transfers"));
    tabs_->setTabText(3, tr("Network"));
    tabs_->setTabText(4, tr("Monitor"));
    tabs_->setTabText(5, tr("Advanced and logs"));
    for (auto& field : fields_) {
        field.label_widget->setText(tr(field.label));
        const auto tip = tr(field.tip);
        for (auto* target :
             {field.widget,
              static_cast<QWidget*>(field.label_widget),
              field.container}) {
            target->setToolTip(tip);
            target->setAccessibleDescription(tip);
        }
        if (field.container != field.widget) {
            for (auto* companion :
                 field.container->findChildren<QAbstractButton*>()) {
                companion->setToolTip(tip);
                companion->setAccessibleDescription(tip);
            }
        }
        if (field.override) {
            field.override->setText(tr("Override environment"));
        }
        if (field.kind == Kind::choice) {
            auto* combo = static_cast<QComboBox*>(field.widget);
            for (std::size_t i = 0; i < field.choices.size(); ++i) {
                combo->setItemText(
                    static_cast<int>(i), tr(field.choices[i].second)
                );
            }
        }
    }
    for (const auto& [label, source] : notes_) {
        label->setText(tr(source));
    }
    for (const auto& [toggle, source] : advanced_) {
        toggle->setText(tr(source));
    }
    for (auto* button : findChildren<QPushButton*>("browseButton")) {
        button->setText(tr("Browse…"));
    }
}

std::vector<config::SettingPatch> SettingsEditor::patches() const {
    std::vector<config::SettingPatch> patches;
    const auto proxy_url =
        std::find_if(fields_.begin(), fields_.end(), [](const auto& f) {
            return f.table == "proxy" && f.key == "url";
        });
    bool remove_proxy = proxy_url->baseline_present;
    if (!proxy_url->override->isChecked()) {
        for (const auto& field : fields_) {
            if (field.table != "proxy" || field.key == "url") {
                continue;
            }
            if ((field.override && field.override->isChecked()) ||
                (!field.override && field.kind != Kind::choice &&
                 !value(field).isEmpty()) ||
                (field.kind == Kind::choice && value(field) != "auto")) {
                remove_proxy = false;
            }
        }
        if (remove_proxy) {
            patches.push_back({"proxy", "", std::monostate{}});
        }
    }
    for (const auto& field : fields_) {
        if (field.table == "proxy" && remove_proxy &&
            !proxy_url->override->isChecked()) {
            continue;
        }
        const auto text = value(field);
        const bool present = !field.override || field.override->isChecked();
        if (text == field.baseline &&
            (!field.override || present == field.baseline_present)) {
            continue;
        }
        config::SettingValue setting = text.toStdString();
        if (field.kind == Kind::number) {
            bool valid = false;
            const auto number = text.toLongLong(&valid);
            const bool digits =
                !text.isEmpty() &&
                std::all_of(text.begin(), text.end(), [](QChar c) {
                    return c >= QChar{'0'} && c <= QChar{'9'};
                });
            if (!valid || !digits || number < 0) {
                throw std::invalid_argument(
                    field.table + "." + field.key +
                    ": expected an integer between 0 and 9223372036854775807"
                );
            }
            setting = static_cast<std::int64_t>(number);
        } else if (field.kind == Kind::boolean) {
            setting = text == "true";
        } else if (!present || ((field.kind == Kind::optional_path ||
                                 field.kind == Kind::optional_text) &&
                                text.isEmpty())) {
            setting = std::monostate{};
        } else if (field.kind == Kind::list ||
                   field.kind == Kind::override_list) {
            std::vector<std::string> entries;
            if (!text.isEmpty()) {
                for (const auto& entry : text.split('\n')) {
                    entries.push_back(entry.toStdString());
                }
            }
            setting = std::move(entries);
        }
        if (field.table == "sync" && field.key == "mode") {
            patches.push_back({"sync", "upload", std::monostate{}});
            // Keep the selected deletion policy when upload-only's implicit
            // default changes.
            const auto policy =
                std::find_if(fields_.begin(), fields_.end(), [](const auto& f) {
                    return f.key == "delete_policy";
                });
            patches.push_back(
                {"sync", "delete_policy", value(*policy).toStdString()}
            );
        }
        if (field.table == "sync" && field.key == "drive_ids") {
            patches.push_back({"sync", "drive_id", std::monostate{}});
        }
        patches.push_back({field.table, field.key, std::move(setting)});
    }
    return patches;
}

} // namespace onedrive::gui
