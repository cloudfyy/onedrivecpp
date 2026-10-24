#include "settings_editor.hpp"
#include <QFormLayout>
#include <QStringList>

namespace onedrive::gui {

void SettingsEditor::add_transfer_page() {
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
}

void SettingsEditor::add_network_page() {
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
}

} // namespace onedrive::gui
