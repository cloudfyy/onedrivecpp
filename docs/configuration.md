# Configuration

English | [简体中文](configuration.zh-CN.md) |
[Documentation index](README.md)

## Configuration file and safety

By default, the application first reads `~/.config/onedrive-cpp/config.toml`. If
that file does not exist, built-in defaults are used. A system-wide example is
installed at `/etc/onedrive-cpp/onedrive-cpp.toml`. To create a user
configuration:

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

Configuration files use TOML and must declare `config_version = 2`. Version 1
files must move their former `sync.download_*` keys into the `transfer` and
`download` tables shown below. Unknown keys and invalid value types are
rejected instead of being silently ignored.

Before running a command, the client secures `state.directory` to owner-only
`0700`, validates the active account token path, and acquires an exclusive
`onedrive-cpp.lock` in that directory. A second process using the same state
directory fails immediately. Existing private state files are tightened to
`0600` when owned by the current user; symbolic links and files owned by
another user are rejected.

For synchronization, `sync.data_directory` and `state.directory` must not contain
one another, the filesystem root cannot be used as the sync directory, and
every existing path component must be free of symbolic links. Normal sync
performs a create/write/fsync/remove probe before contacting Graph. Dry-run
keeps its non-mutating sync-directory behavior. Downloads also reserve the
larger of 256 MiB or 5% of the actual transfer as a safety margin. Concurrent
workers reserve only their remaining bytes before transfer, release promised
space at each durable checkpoint, and wait when another active download owns
the currently available capacity. This permits large batches to proceed
sequentially without allowing concurrent downloads to overcommit the disk.

`sync.drive_id` selects the remote OneDrive drive to access. The default value,
`me`, selects the signed-in user's default OneDrive and lists its root through
the Microsoft Graph path `/me/drive/root/children`. To access another OneDrive
or a SharePoint document library available to the account, set the actual
Drive ID instead; the client then uses `/drives/<drive_id>/root/children`.
## Reference configuration

The following example shows every major configuration group:

```toml
[console]
# "auto", "console", or "tui"; applies to sync and can be overridden by --ui.
ui = "auto"
# "auto", "always", or "never"
color = "auto"

[sync]
# Default OneDrive of the signed-in user
data_directory = "/home/USER/OneDrive"
drive_id = "me"
permissions = "private"
local_conflict = "block"
# Optional mounted ancestor; Monitor also checks it before every sync
# data_mount_point = "/mnt/data"
# Optional; resolved relative to this TOML file
# sync_list = "sync_list"
sync_root_files = false
# A regular .nosync file excludes its directory subtree.
nosync_enabled = true
# "include" or "exclude"
dotfiles = "include"
# Zero means unlimited.
maximum_file_size_bytes = 0
mode = "bidirectional"
delete_policy = "propagate"
maximum_remote_deletions = 1000

# Another OneDrive or SharePoint document library
# drive_id = "b!YOUR_DRIVE_ID"

# Optional; applies to authentication, Graph API, and file downloads
# [proxy]
# url = "socks5h://127.0.0.1:1080"
# no_proxy = ["localhost", "127.0.0.1", ".internal.example.com"]
# username = "proxy-user"
# password_file = "/run/secrets/onedrive-proxy-password"
# auth = "auto"
# ca_file = "/etc/ssl/certs/company-proxy-ca.pem"

[transfer]
order = "default"
connect_timeout_seconds = 30
operation_timeout_seconds = 3600
stall_timeout_seconds = 60
stall_minimum_bytes_per_second = 1
http_version = "auto"
ip_version = "auto"

[download]
concurrency = 4
maximum_retries = 4
chunk_threshold_bytes = 8388608
checkpoint_interval_bytes = 1048576
maximum_rate_bytes_per_second = 0
maximum_total_rate_bytes_per_second = 0
validation = "strict"

[upload]
# Independent files can upload concurrently; each upload session remains
# sequential.
concurrency = 1
# Files through 250 MB (250,000,000 bytes) use a simple upload. Larger files
# use an upload session with the fragment size below.
chunk_size_bytes = 10485760
maximum_rate_bytes_per_second = 0
maximum_total_rate_bytes_per_second = 0

[monitor]
websocket_enabled = true
websocket_request_timeout_seconds = 60
websocket_connect_timeout_seconds = 10
websocket_renewal_lead_seconds = 120
websocket_initial_backoff_seconds = 1
websocket_maximum_backoff_seconds = 300
poll_interval_seconds = 300
settle_delay_milliseconds = 1000
```

## Storage and mount safety

Set `sync.data_mount_point` when `sync.data_directory` resides on removable
storage, a network filesystem, or another mount that may disappear. The
configured path must be a mounted directory and an ancestor of
`sync.data_directory`. The client checks it during startup and again before
every full or single-file synchronization. If the mount disappears while
Monitor is running, the next synchronization stops before local scanning,
pending-operation recovery, downloads, uploads, remote deletions, or Delta
cursor updates. Monitor remains running and retries on a later scheduled
synchronization, so normal operation resumes after the same path is mounted
again.

This option is disabled by default and does not infer a mount from
`sync.data_directory`. For the layout `/mnt/data/OneDrive`, where `/mnt/data` is the
actual mount point, configure:

```toml
[sync]
data_directory = "/mnt/data/OneDrive"
data_mount_point = "/mnt/data"
```

## Large-delete protection

`sync.maximum_remote_deletions` limits how many tracked items one local
deletion plan may remove from OneDrive. The default is `1000`; `0` blocks every
remote deletion unless explicitly overridden. A missing tracked directory
counts itself and every tracked descendant it would remove, even though Graph
needs only one DELETE request for the parent. The client checks the complete
batch before upload-side moves, directory creation, uploads, or deletes, and
checks it again immediately before deletion execution.

When the limit is exceeded, normal sync and monitor runs stop before issuing a
remote DELETE. Monitor never bypasses the guard. `sync --dry-run` reports the
Graph operation count, affected tracked-item count, configured limit, and
whether a normal run would be blocked. After reviewing the local filesystem,
an intentional one-shot batch can be approved with:

```bash
onedrive-cpp sync --force-large-delete
```

The override applies only to that invocation and cannot be persisted in the
configuration file. Existing pending-delete recovery is protected by the same
limit, so restarting the process cannot bypass the safeguard.

## Monitor scheduling and notifications

`monitor` performs an initial synchronization, acquires a Microsoft Graph
Socket.IO channel, and then remains idle until a remote WebSocket notification,
an inotify local-change event, or the Graph polling interval expires.
Local event bursts are coalesced for `monitor.settle_delay_milliseconds`.
`monitor.poll_interval_seconds` bounds how long remote changes can remain
undetected if notification acquisition, renewal, or delivery fails. WebSocket
notifications are advisory wakeups; each one runs an authoritative Delta query,
and reconnecting also schedules a catch-up query. Newly created and moved
directory trees are watched recursively; an inotify queue overflow rebuilds
every watch and schedules a complete synchronization. `SIGINT` and `SIGTERM`
wake the blocking wait and stop cleanly after the active synchronization
finishes.
Set `monitor.websocket_enabled = false` to disable Graph Socket.IO/WSS
notifications; local inotify events and periodic Graph polling remain active.
The request and connect timeouts bound channel acquisition and the WSS
handshake. The renewal lead refreshes a channel before expiry, while the
initial and maximum backoff values bound exponential retries. Engine.IO
heartbeat timing is negotiated by the server and is intentionally not
configurable.
The monitor scheduler is an explicit single-threaded runtime state machine with
starting, idle, local-settling, synchronizing, and stopped states. Local bursts
reset the settle deadline, queue overflow upgrades the pending reason, and a
pending local settle remains ahead of an expired Graph poll. System I/O and
`SyncEngine` stay outside the state reducer, so the scheduler constrains event
ordering without duplicating synchronization policy.

## Selection and filtering

`sync.sync_list` enables client-side selective synchronization. It names a
separate UTF-8 rule file; relative paths are resolved from the directory
containing the TOML configuration file. If the setting is absent, all remote
items are eligible for synchronization. If it is present, the file must be
readable, and an empty rule file selects no remote items.

The rule file excludes everything by default and supports:

- blank lines and lines beginning with `#`;
- inclusion rules such as `/Documents/` or `Pictures/*.jpg`;
- exclusion rules beginning with `!` or `-`;
- a leading `/` to anchor a rule at the Drive root;
- a trailing `/` to restrict a rule to directories and their descendants;
- `*` within one path segment and `**` as a complete recursive segment.

For example:

```text
# Include Documents but omit private content and temporary files
/Documents/
!/Documents/Private/*
!/Documents/**/*.tmp

# Include matching pictures at any directory depth
Pictures/*.jpg
```

Exclusions override inclusions. The client retains the directory ancestors
needed to materialize selected files. Rules without a leading `/` may match at
any depth and therefore have broader semantics. Backslashes, empty path
components, `.` and `..`, and `**` embedded within another segment are
rejected.

Filtering is performed after Microsoft Graph returns Delta metadata; it
reduces local materialization and file transfers but does not provide
server-side Graph filtering. A fingerprint of the effective rules is committed
atomically with the Delta cursor. Adding, changing, removing, or reordering
rules automatically causes the next synchronization to fetch the full remote
state. Excluded files already present locally are deliberately retained because
selection changes are not remote deletion records. When a tracked file moves
from an included path to an excluded path, schema-v16 SQLite state binds an
upload suppression to the retained object's device/inode identity. The same
object is not uploaded again from its old path, even after local modification.
If it disappears or a different filesystem object replaces it, the stale
suppression is removed during the next upload scan. Moving the remote item back
into the selected set downloads its current path without releasing protection
for the retained old copy. The explicit `download REMOTE_PATH` command is not
restricted by `sync.sync_list`.

When `sync.sync_list` is configured, `sync.sync_root_files = true`
automatically includes ordinary files located directly in the Drive root.
Root directories and their descendants still require an inclusion rule, and
an exclusion rule such as `!/root-secret.txt` overrides the automatic
inclusion. The default is `false`; without `sync.sync_list`, the setting has no
effect because normal synchronization already includes all files. Changing
this value changes the selective-sync fingerprint and therefore triggers a
full remote-state query before the new selection is committed.

The same filter also applies to downloads, uploads, local-move discovery, and
remote-deletion planning:

- `sync.nosync_enabled = true` (the default) treats a regular `.nosync` file as
  a local marker that excludes the containing directory and its complete
  subtree. The marker itself is never synchronized. Symbolic links named
  `.nosync` are not markers.
- `sync.dotfiles = "include"` keeps Unix dotfiles eligible for synchronization.
  Set it to `"exclude"` to exclude any path having a component that begins
  with `.` and contains at least one additional character.
- `sync.maximum_file_size_bytes = 0` imposes no size limit. A positive value
  excludes regular files larger than the limit in either direction; a file
  exactly equal to the limit remains eligible.

Filtered local absence is not interpreted as a remote deletion. Changes to
these policies, and additions or removals of `.nosync` markers, update the
filter fingerprint and force a full Delta query. Existing local files that
become excluded are retained with identity-based upload suppression. If a file
later becomes eligible again, normal local-conflict protection still applies.
The explicit `download REMOTE_PATH` command remains outside these automatic
synchronization filters.

## Conflict handling

`sync.local_conflict` controls simultaneous local and remote file changes.
The default, `"block"`, preserves the existing behavior: synchronization
records the item as `local_modification`, and explicit single-file download
stops without downloading. Set it to `"backup"` to copy the stable local
contents to a durable same-directory name such as
`report.safeBackup-20261004T051000Z-0001.pdf` before atomically installing the
authoritative remote version. The copy is independent rather than a hard link,
retains the local permission bits, and is excluded from upload. If local and
downloaded contents have the same SHA-256
fingerprint, the existing file is adopted without creating a backup or
replacing its inode. Backup creation requires additional disk space equal to
the local file and fails safely without replacing the destination. This policy
applies to normal synchronization, `download REMOTE_PATH`, and an upload
recovery that discovers a simultaneous remote change. Such a recovery
discards only its stale upload snapshot and journal, then reconciles the
remote delta through the same policy. Directory and remote-deletion conflicts
that cannot produce a safe regular-file copy remain blocked.

For tracked files, the state database stores both Graph eTag and cTag values.
When the local snapshot is unchanged and a delta changes only the eTag while
retaining the same non-empty cTag, synchronization refreshes the remote
metadata without downloading the file again. A missing or changed cTag falls
back to the conservative eTag behavior and downloads the remote content.
Folder decisions do not rely on cTag because SharePoint and OneDrive for
Business may omit it or report descendant changes inconsistently.

## Synchronization modes and deletion policies

`sync.mode` defaults to `bidirectional`, which downloads remote changes and
uploads local changes selected by the same sync-list rules. `upload_only`
still fetches and records the Graph Delta baseline, but never downloads,
moves, or deletes local content. New and modified local files continue through
the normal conflict checks and durable upload journal. `download_only`
downloads remote changes but does not scan local changes for upload and does
not recover pending upload, remote-move, or remote-delete operations.

With `download_only`, `sync.delete_policy = "propagate"` safely removes an
unchanged tracked local item after its remote source is deleted. A local
modification, unexpected type, or nonempty directory is blocked and retried
through the existing blocked-item journal. `preserve` keeps the local item but
consumes the remote tombstone, removes its tracked state, and clears an older
blocked deletion so full Delta refreshes do not repeatedly plan the same
removal. Pending downloads still recover before applying the latest remote
Delta.

Set `sync.delete_policy = "preserve"` to prevent a missing local item from
deleting its remote counterpart; this is also the safe default when
`upload_only` is selected without an explicit policy. `propagate` remains the
default for bidirectional synchronization. The legacy `sync.upload` boolean
remains accepted for compatibility (`true` maps to `bidirectional`, `false`
to `download_only`) but cannot be combined with `sync.mode`.

New files use fail-on-conflict creation; tracked files use their saved eTag as
an `If-Match` precondition. Symbolic links, reserved safeBackup and
transfer-temporary names, blocked remote paths, and type conflicts are never
uploaded. Each transfer uses a stable private snapshot and a durable SQLite
pending-upload journal. Recovery verifies an already-created remote file by
downloading it and comparing SHA-256 before committing state.
Selected untracked local directories are created remotely in parent-first
order before their files. Directory creation uses fail-on-conflict semantics
and the same durable journal. A definite Graph conflict removes the journal
and blocks the operation; after an ambiguous interruption, recovery retries
the request and adopts an existing item only when it is a directory at the
exact expected path.
## Uploads, moves, and remote mutations

Files through 250 MB use a simple upload. Larger files use a Microsoft Graph
upload session with contiguous fragments and advance only to the exact
`nextExpectedRanges` offset confirmed by Graph. The default fragment size is
10 MiB; non-final fragments are a multiple of 320 KiB and remain below Graph's
60 MiB request limit. Upload-session URLs are preauthorized and therefore never
receive the Graph Authorization header or appear in logs. The pending-upload
journal persists the session URL, expiration, and each offset confirmed by
Graph. After a restart, the client queries the session without an Authorization
header, accepts Graph progress ahead of the last local checkpoint, and resumes
without resending confirmed fragments. Expired sessions and HTTP 404/410
responses create a new session; a server offset behind the durable checkpoint
stops the upload instead of risking duplicate data. Legacy
`upload = false` maps to `mode = "download_only"`.

OneDrive quota responses and local upload read, permission, snapshot-space,
or I/O failures are recorded in the same pending-upload journal with an
actionable reason and attempt count. One failed item does not stop unrelated
uploads. The client retries each recorded failure once per synchronization;
successful recovery clears the journal, while repeated failures remain
visible in warnings and the blocked summary.

Missing tracked local items are deleted remotely with their saved eTag as an
`If-Match` precondition. Directory deletions are parent-first and cover their
tracked descendants. A dedicated SQLite journal makes an already-completed
Graph deletion recoverable after a process interruption; HTTP 404 is therefore
an idempotent success, while 409/412 stops without discarding tracked state.
Dry-run and paths outside the active sync-list never issue deletions.
Tracked local files and directories also persist their filesystem device and
inode identity. Downloads and uploads record it immediately, while a normal
non-dry-run upload scan backfills older snapshots. This stable identity is the
basis for safely recognizing local moves without relying on ambiguous size and
timestamp matches. A recognized move uses the saved eTag in a conditional
Graph PATCH and a dedicated SQLite journal. File modifications made together
with a move are uploaded after the move commits. Directory moves remap tracked
descendants atomically. Restart recovery adopts the destination only when its
remote ID and local filesystem identity both match. The destination parent
may be newly created locally: synchronization creates each missing remote
parent from shallowest to deepest before issuing the conditional move. Parent
creation and the move retain their separate durable journals.

## Cloud endpoints and proxy

`graph.endpoint` selects the Microsoft Graph cloud endpoint and defaults to
the global service. It is not tied to a specific SharePoint host. For a
SharePoint library in the Microsoft 365 China cloud, configure both the Graph
and authentication cloud endpoints:

```toml
[auth]
endpoint = "https://login.chinacloudapi.cn"

[graph]
endpoint = "https://microsoftgraph.chinacloudapi.cn/v1.0"
```

The optional `proxy.url` setting routes authentication, Microsoft Graph, and
file-download requests through one proxy. Supported URL schemes are `http`,
`https`, `socks4`, `socks4a`, `socks5`, and `socks5h`. Use `socks5h` rather
than `socks5` when destination host names must be resolved by the proxy:

```toml
[proxy]
url = "https://proxy.example.com:8443"
no_proxy = ["localhost", "127.0.0.1", ".internal.example.com"]
username = "proxy-user"
password_file = "/run/secrets/onedrive-proxy-password"
auth = "auto"
ca_file = "/etc/ssl/certs/company-proxy-ca.pem"

# Or use remote DNS through SOCKS5:
# url = "socks5h://127.0.0.1:1080"
```

The complete proxy configuration defaults are:

- `proxy.url`, `proxy.no_proxy`, `proxy.username`, `proxy.password_file`, and
  `proxy.ca_file` are unset;
- `proxy.auth` defaults to `auto`;
- because `proxy.url` is unset, the application does not explicitly select a
  proxy, but libcurl can still use `HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`,
  and their lowercase variants from the environment;
- because `proxy.no_proxy` is unset, libcurl uses `NO_PROXY`/`no_proxy` from
  the environment;
- without `proxy.ca_file`, HTTPS proxies use the system CA trust store.

To guarantee direct connections, omit the `[proxy]` table and unset all proxy
environment variables. To use a configured proxy for every destination
regardless of `NO_PROXY`, set `no_proxy = []`.

`proxy.no_proxy` is an array of hosts, domain suffixes, IP addresses, or other
libcurl no-proxy patterns. When omitted, libcurl's `NO_PROXY`/`no_proxy`
environment variable remains effective. An explicitly empty array overrides
the environment and sends every destination through the proxy.

`proxy.username` enables proxy credentials. Store the corresponding password
in `proxy.password_file` instead of the TOML file or proxy URL. Relative
password paths resolve from the TOML file's directory. The password file must
be a regular file reached without symbolic links, must grant no group or other
permissions, and must not exceed 64 KiB. One trailing LF or CRLF is removed.
An empty password, embedded NUL byte, or password file without a username is
rejected.

`proxy.auth` controls HTTP/HTTPS proxy authentication and accepts `auto`,
`basic`, `digest`, `ntlm`, or `negotiate`; it defaults to `auto`. SOCKS5 uses
its own username/password authentication. Authentication mechanisms actually
available depend on the installed libcurl build.

`proxy.ca_file` adds a certificate-authority file for an HTTPS proxy and is
rejected for other proxy schemes. Relative paths resolve from the TOML file's
directory. Proxy certificate and host verification remain enabled and cannot
be disabled by configuration. Keep credentials out of `proxy.url` because
configuration or error diagnostics may expose URLs.

## Transfer behavior and limits

`download.concurrency` controls how many files can be downloaded at the same
time. It defaults to `4` and accepts values from `1` through `16`. Downloads
targeting the same normalized local path are always serialized, including
common ASCII case-only path variants, while unrelated destinations remain
concurrent.

`transfer.order` controls the order in which file transfers enter the worker
queue. Supported values are `default`, `size_asc`, `size_dsc`, `name_asc`, and
`name_dsc`. The default preserves synchronization-plan order. Equal sort keys
also preserve plan order. With concurrent workers this controls start order,
not completion order. The setting applies to the download queue; upload
dependencies retain their parent-first operation order.

`download.chunk_threshold_bytes` sets the large-file threshold in bytes. Files
larger than this value are downloaded sequentially with HTTP byte-range
requests, using the same value as the maximum chunk size. It defaults to
`8388608` (8 MiB) and must be greater than zero. Files at or below the
threshold use a single request. Range response metadata is validated before
response bytes are written. Single-request and relaxed downloads likewise
reject non-success HTTP response bodies before they can reach the temporary
file or a durable checkpoint. The Graph content request uses the expected
remote eTag as an `If-Match` precondition, so a changed remote version is
rejected before a download URL is issued. Large transfers periodically flush durable
checkpoints so an interrupted request retries from the last safely stored
offset instead of the beginning of the chunk. Graceful cancellation also
flushes and records bytes from an already validated Range response before
stopping.

`download.checkpoint_interval_bytes` controls how many newly downloaded bytes
are written durably before resumable progress is recorded. It defaults to
`1048576` (1 MiB) and must be greater than zero. Smaller values reduce
re-download work after interruptions but increase synchronization and database
overhead.

`download.maximum_retries` controls how many times a file-content request is
retried after a transient HTTP or transport failure. It defaults to `4`;
`0` disables file-content retries. This is independent of
`graph.throttle.maximum_retries`, which continues to control Microsoft Graph
API request retries. Download retries use the Graph backoff delay settings.

Shared `transfer` settings control each file-content request and are also
used by uploads. Connection and operation timeouts default to `30`
and `3600` seconds. A transfer that remains below
`transfer.stall_minimum_bytes_per_second` (default `1`) for
`transfer.stall_timeout_seconds` (default `60`) is aborted; set the stall
timeout to `0` to disable this check.
`download.maximum_rate_bytes_per_second` limits each individual file-content
request and defaults to `0`, meaning unlimited.
`download.maximum_total_rate_bytes_per_second` limits the combined receive
rate shared by all concurrent file downloads and also defaults to `0`.
When both are non-zero, each request is subject to the individual limit while
all active requests share the total limit. The total limiter uses a fair,
cancellable token bucket with a burst of at most 64 KiB. Throttling time counts
toward `transfer.operation_timeout_seconds` and may contribute to libcurl's
stall detection, so very low rate limits may require a longer operation or
stall timeout.
`upload.maximum_rate_bytes_per_second` limits each upload request's send rate
and defaults to `0`. `upload.concurrency` controls how many independent files
may upload concurrently, defaults to `1`, and accepts `1` through `16`.
Fragments within one Microsoft Graph upload session always remain sequential.
Directories are created before dependent files enter the worker queue.
`upload.maximum_total_rate_bytes_per_second` supplies a combined ceiling shared
by all active uploads and defaults to `0`. It uses the same fair, cancellable
token-bucket implementation as aggregate download limiting. The default
`upload.chunk_size_bytes` is 10 MiB and must be a positive multiple of 320 KiB
below 60 MiB.
`transfer.http_version` accepts `"auto"`, `"1.1"`, or `"2"`; HTTP/2 is
negotiated over TLS and may fall back according to libcurl capabilities.
`transfer.ip_version` accepts `"auto"`, `"4"`, or
`"6"` and defaults to `"auto"`; forcing an address family can work around
broken IPv6 or IPv4 routing, but fails when the download host has no address in
that family. Preauthenticated download URLs can follow at most five additional
redirects, all of which must use HTTPS; Graph authorization is never attached
to those CDN requests. Each download worker safely reuses its reset libcurl easy handle,
allowing DNS, TCP, TLS, and HTTP/2 connection state to be reused across chunks,
retries, and subsequent files without carrying request headers, bodies, or
callbacks between operations. Trace logging reports the negotiated HTTP
version, number of newly opened connections, and DNS, TCP, TLS, server wait,
body-transfer, and total timings in microseconds. These diagnostics do not
include request URLs, headers, or bodies.

Download progress reports aggregate all active files and include the current
smoothed transfer rate and estimated time remaining. The final report includes
the total elapsed download time. JSON progress events expose the same values as
`bytes_per_second`, `estimated_seconds_remaining`, and
`elapsed_milliseconds`.

`download.validation` defaults to `"strict"`, which requires downloaded size
and any Graph-provided content hash to match the remote metadata. Some
SharePoint, Azure Information Protection (AIP), and HEIC files are served with
bytes that differ from their Graph metadata. `"relaxed"` accepts those files,
but disables resumable/chunked downloads and remote size/hash verification.
HTTP success, durable writes, atomic installation, and the local SHA-256
recovery fingerprint remain enforced. Disk space is reserved incrementally
from actual transfer progress rather than untrusted Graph size metadata, and
the transfer is aborted if the reservation cannot grow safely. Because Graph
cannot reliably identify AIP-protected files in advance, relaxed mode applies
to all downloads and weakens integrity guarantees.

## Permissions and storage layout

`permissions` defaults to `"private"`. New synchronized files are created as
`0600`, and the synchronization root and new directories are secured as
`0700`, so other local users cannot read synchronized content. Set it to
`"umask"` only when synchronized data must intentionally follow the process
umask, such as a directory shared through Unix group permissions. The packaged
systemd user service also uses `UMask=0077` as defense in depth.

`sync.data_directory` is the common root for synchronized data. Actual Drive
contents are isolated with the same stable, friendly account and Drive
components used by state:

```text
<sync.data_directory>/accounts/<display-name>--<user-id-hash>/
  drives/<drive-name>--<drive-id-hash>/
    <synchronized OneDrive contents>
```

This allows one configured root to hold multiple Microsoft users and multiple
Drives without path collisions. Existing files in the former flat
`<sync.data_directory>` layout are not moved automatically and remain untouched.

State is separated by the stable Microsoft user ID and canonical Drive ID while
retaining friendly directory names:

```text
<state.directory>/accounts/<display-name>--<user-id-hash>/
  account.json
  avatar.<image-extension>
  refresh_token
  drives/<drive-name>--<drive-id-hash>/
    drive.json
    items.sqlite3
```

The account and Drive metadata use stable ID hashes so display-name changes do
not create a second state tree. The Drive database stores and validates the
user ID, display name, canonical Drive ID, Drive name, profile-photo MIME type,
and profile-photo bytes in addition to remote IDs, ETags, and local paths.
Its `drive_mapping` table records the configured selector and resolved ID, for
example `me` to the canonical Microsoft Drive ID. SQLite uses WAL mode.

The former flat `<state.directory>/items.sqlite3` and
`<state.directory>/refresh_token` layout is intentionally not migrated. Run
`onedrive-cpp auth` again to initialize the account-based layout and rebuild
synchronization state.
