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
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.application_id); });
    add(account,
        "auth",
        "tenant_id",
        QT_TR_NOOP("Tenant ID"),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.azure_tenant_id);
        });
    add(
        account,
        "sync",
        "data_directory",
        QT_TR_NOOP("Sync directory"),
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
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.state_directory.string());
        },
        {},
        true
    );
    add(account,
        "sync",
        "drive_id",
        QT_TR_NOOP("Drive ID"),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.drive_id); });
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
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.auth_endpoint); });
    add(account_advanced,
        "graph",
        "endpoint",
        QT_TR_NOOP("Graph endpoint"),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.graph_endpoint); });
    add(account_advanced,
        "auth",
        "scopes",
        QT_TR_NOOP("Authentication scopes (one per line)"),
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
        Kind::optional_path,
        [](const auto& c) {
            return c.sync_list ? QString::fromStdString(c.sync_list->string())
                               : QString{};
        });
    add(sync,
        "sync",
        "dotfiles",
        QT_TR_NOOP("Dotfiles"),
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
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_file_size_bytes);
        });
    auto* sync_advanced = advanced(sync, QT_TR_NOOP("Advanced sync rules"));
    add(sync_advanced,
        "sync",
        "nosync_enabled",
        QT_TR_NOOP("Honor .nosync files"),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.nosync_enabled ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "sync_root_files",
        QT_TR_NOOP("Include root files with selective sync"),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.sync_root_files ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "maximum_remote_deletions",
        QT_TR_NOOP("Maximum remote deletions (0: no deletions)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_remote_deletions);
        });

    auto* transfer = page();
    add(transfer,
        "upload",
        "concurrency",
        QT_TR_NOOP("Upload concurrency (1–16)"),
        Kind::number,
        [](const auto& c) { return QString::number(c.upload_concurrency); });
    add(transfer,
        "download",
        "concurrency",
        QT_TR_NOOP("Download concurrency (1–16)"),
        Kind::number,
        [](const auto& c) { return QString::number(c.download_concurrency); });
    add(transfer,
        "upload",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Upload rate per request (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "download",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Download rate per request (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "upload",
        "maximum_total_rate_bytes_per_second",
        QT_TR_NOOP("Aggregate upload rate (bytes/s; 0: unlimited)"),
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
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_chunk_size_bytes);
        });
    add(transfer_advanced,
        "download",
        "chunk_threshold_bytes",
        QT_TR_NOOP("Download chunk threshold (bytes; greater than 0)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_chunk_threshold_bytes);
        });
    add(transfer_advanced,
        "download",
        "checkpoint_interval_bytes",
        QT_TR_NOOP("Download checkpoint interval (bytes; greater than 0)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_checkpoint_interval_bytes);
        });
    add(transfer_advanced,
        "download",
        "maximum_retries",
        QT_TR_NOOP("Maximum download retries"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_retries);
        });
    add(transfer_advanced,
        "download",
        "validation",
        QT_TR_NOOP("Download validation"),
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
        Kind::optional_text,
        [](const auto& c) {
            return c.proxy.username ? QString::fromStdString(*c.proxy.username)
                                    : QString{};
        });
    add(network,
        "proxy",
        "password_file",
        QT_TR_NOOP("Proxy password file (empty: unset)"),
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
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_maximum_throttle_retries);
        });
    add(network_advanced,
        "graph.throttle",
        "initial_delay_seconds",
        QT_TR_NOOP("Initial throttle delay (seconds)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_initial_throttle_delay.count());
        });
    add(network_advanced,
        "graph.throttle",
        "maximum_delay_seconds",
        QT_TR_NOOP("Maximum throttle delay (seconds; at least initial delay)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.graph_maximum_throttle_delay.count());
        });

    auto* monitor = page();
    add(monitor,
        "monitor",
        "websocket_enabled",
        QT_TR_NOOP("Enable WebSocket notifications"),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.monitor_websocket_enabled ? "true" : "false"};
        });
    add(monitor,
        "monitor",
        "poll_interval_seconds",
        QT_TR_NOOP("Poll interval (seconds; 1–86400)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_poll_interval.count());
        });
    add(monitor,
        "monitor",
        "settle_delay_milliseconds",
        QT_TR_NOOP("Local settle delay (milliseconds; 0–60000)"),
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
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_request_timeout.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_connect_timeout_seconds",
        QT_TR_NOOP("WebSocket connection timeout (seconds; 1–300)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_connect_timeout.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_renewal_lead_seconds",
        QT_TR_NOOP("WebSocket renewal lead (seconds; 0–3600)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.monitor_websocket_renewal_lead.count());
        });
    add(monitor_advanced,
        "monitor",
        "websocket_initial_backoff_seconds",
        QT_TR_NOOP("WebSocket initial backoff (seconds; 1–3600)"),
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
        "logging",
        "level",
        QT_TR_NOOP("Logging level"),
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
         kind,
         widget,
         label_widget,
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
        patches.push_back({field.table, field.key, std::move(setting)});
    }
    return patches;
}

} // namespace onedrive::gui
