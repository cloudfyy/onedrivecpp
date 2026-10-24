#include "settings_editor.hpp"
#include <QFormLayout>
#include <QStringList>

namespace onedrive::gui {

void SettingsEditor::add_monitor_page() {
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
}

void SettingsEditor::add_diagnostics_page() {
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
}

} // namespace onedrive::gui
