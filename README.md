# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client scaffold for
Ubuntu 24.04 LTS and later. Its separation of responsibilities is inspired by
[abraunegg/onedrive](https://github.com/abraunegg/onedrive), but it does not
copy the reference project's D implementation.

> The current version provides a buildable architecture scaffold,
> configuration loading, a CLI, Microsoft device-code authentication, an HTTP
> transport layer, authenticated Microsoft Graph delta queries, SQLite remote
> state and delta-link persistence, safe one-way downloads, dry-run support, a
> systemd user service, and Debian packaging. Synchronization currently
> creates remote directories and downloads added or changed remote files;
> uploads, local execution of remote deletions, and two-way conflict resolution
> are not implemented.

## Architecture

```text
main (composition root)
       |
       +-- ProductionRuntimeFactory
               |
               +-- Application (CLI and command orchestration)
                       |
                       +-- HttpTransport / TokenStore
                       +-- FileMonitor / Metrics
                       +-- SyncEngine
                               |
                               +-- GraphClient
                               +-- ItemStore
```

The application uses constructor injection and explicit port interfaces rather
than a service locator. `main` is the only composition root. The production
runtime factory creates the libcurl, file-token, SQLite, monitor, Graph, and
metrics adapters after configuration is loaded; tests inject in-memory fakes.
This keeps business orchestration independent of infrastructure and leaves a
stable metrics port for a future Linux metrics exporter.
Runtime ports use Proxy 4 type erasure, so adapters satisfy the required
operations without inheriting from project-owned abstract base classes.
The SQLite ItemStore owns a dedicated database thread. Calls from download,
monitor, and future upload workers are queued and completed synchronously, so
one thread owns the SQLite connection and transaction order while errors are
returned to the caller. SQLite is the authoritative state source; the adapter
does not maintain duplicate mutable item caches.

The directories correspond to the responsibilities of
`main/config/curlEngine/onedrive/sync/itemdb/monitor` in the reference project:

- `src/app`: CLI parsing, application lifecycle, and runtime dependency factory.
- `src/auth`: Device-code OAuth, token refresh, and secure token persistence.
- `src/config`: Configuration file loading and validation.
- `src/graph`: Microsoft Graph API boundary.
- `src/http`: Typed libcurl HTTP transport.
- `src/storage`: Item-store port and SQLite-persisted remote ID, ETag, and local path adapter.
- `src/sync`: Difference calculation and synchronization orchestration.
- `src/monitor`: Long-running monitor mode entry point.
- `src/metrics`: Metrics port and no-op production adapter.
- `packaging/systemd`: systemd user service.
- `debian`: Native Ubuntu/Debian package metadata.

## Supported Platforms

- Ubuntu 24.04 LTS and later.
- x86_64 or arm64, depending on the LLVM and Ubuntu build environment.
- Clang 20 with `-std=c++2c` / CMake `CXX_STANDARD 26`.
- CMake 3.28 or later.
- Development packages for CLI11, libcurl/OpenSSL, nlohmann/json, spdlog/fmt,
  SQLite, and toml++ from the system package manager.

Ubuntu 24.04 provides the required CMake, Ninja, and Clang 20 packages through
its official repositories. GCC 13 is not used because its C++26 support is not
sufficient for this project configuration.

## Set Up the Build Environment

Install the build tools from the Ubuntu 24.04 repositories:

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 clang-tidy-20 zip unzip tar pkg-config \
  libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

Verify the installed versions:

```bash
clang++-20 --version
cmake --version
```

The compiled libraries are resolved from the operating system and linked
dynamically. This lets Debian security updates replace libcurl, OpenSSL,
SQLite, spdlog, and fmt without rebuilding `onedrive-cpp`.
Proxy 4 is header-only. CMake uses an installed `msft_proxy4` package when
available and otherwise downloads the pinned ngcpp/proxy 4.1.0 release.
The vcpkg manifest resolves it through the `proxy` port.

## Build and Test Locally

After cloning the repository, change to its root directory. All build commands
below assume the project root is the current working directory.

### Release build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The executable is generated at `build/release/onedrive-cpp`. Verify it with:

```bash
./build/release/onedrive-cpp --version
./build/release/onedrive-cpp sync --dry-run
./build/release/onedrive-cpp --help
```

### Logging

Runtime diagnostics are written to standard error with `info` severity by
default. systemd captures that stream automatically:

```bash
journalctl --user -u onedrive-cpp.service -f
```

Every subcommand accepts diagnostic logging and user-output options:

```bash
onedrive-cpp sync --dry-run --log-level debug
onedrive-cpp monitor --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
onedrive-cpp sync --dry-run --color always
onedrive-cpp sync --dry-run --output json
onedrive-cpp sync --dry-run --quiet
```

Supported levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`,
and `off`. A configured log file rotates at 5 MiB and retains three older
files. Authentication tokens, device codes, and authorization headers must
never be written to logs.

`--color=auto` is the default: styling is enabled only for a terminal and is
disabled when `NO_COLOR` is set. `always` forces ANSI styling and `never`
disables it. `--output=json` emits one compact JSON object per line and never
emits ANSI sequences; destructive interactive confirmation requires `--yes`
in this mode. `--quiet` suppresses informational and success output while
retaining warnings and errors. Diagnostic logs remain on standard error, while
command results are written to standard output.

### Shell completion

The package installs Bash and Zsh completion definitions for subcommands,
options, enumerated values, and file paths. New shell sessions load them
automatically when the shell completion system is enabled.

For a development build, enable Bash completion in the current shell with:

```bash
source packaging/completions/onedrive-cpp.bash
```

Then use Tab completion for commands and values:

```bash
build/release/onedrive-cpp <Tab>
build/release/onedrive-cpp sync --<Tab>
build/release/onedrive-cpp sync --log-level <Tab>
```

The installed definitions are placed in
`share/bash-completion/completions/onedrive-cpp` and
`share/zsh/vendor-completions/_onedrive-cpp`.

### Manual page

The CMake install rules place the section 1 manual at the standard
`share/man/man1` location. After installing the DEB package, view it with:

```bash
man onedrive-cpp
```

Debian's `man-db` trigger updates the index automatically. After a direct
`cmake --install` into a custom prefix, refresh the local index when needed:

```bash
sudo mandb
```

The generated page can also be inspected without installing it:

```bash
man --local-file build/release/generated/onedrive-cpp.1
```

### Clean Release rebuild

Delete only the Release build tree to force CMake and Ninja to reconfigure and
rebuild it from scratch:

```bash
rm -rf build/release
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/onedrive-cpp sync --dry-run
```

Ninja builds in parallel automatically. To set an explicit job count:

```bash
cmake --build --preset release --parallel 4
```

### Debug build

The Debug tree is independent of the Release tree:

```bash
rm -rf build/debug
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The Debug executable is generated at `build/debug/onedrive-cpp`.

### C++ Core Guidelines checks

The lint preset runs Clang-Tidy during compilation with the project
`.clang-tidy` policy. Diagnostics from the Clang static analyzer, bug-prone,
performance, portability, and selected C++ Core Guidelines checks are treated
as build errors:

```bash
cmake --preset lint
cmake --build --preset lint
```

The policy excludes only reviewed noise from required C/POSIX APIs, protocol
constants, and checked buffer boundaries. Microsoft GSL is used to express
non-null borrowed dependencies at API and RAII boundaries. ngcpp/proxy
provides type-erased runtime ports with explicit owning and borrowed adapters.

## Build DEB Packages

### Quick CPack package

Use CPack to quickly create a package that has not undergone full Debian
policy checks:

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

The generated package is placed in the current directory. CMake reads
`ID` and `VERSION_ID` from `/etc/os-release`, so an Ubuntu 24.04 amd64 build
is named:

```text
onedrive-cpp_0.5.0-1~ubuntu24.04_amd64.deb
```

For a cross-distribution build environment, override the detected suffix
explicitly during configuration:

```bash
cmake --preset release \
  -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04
```

Packages for Ubuntu 26.04 must still be built and tested in an Ubuntu 26.04
environment; changing the suffix does not change the binary ABI.

### Official Debian package

Install the packaging dependencies:

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

Run the following commands from the project root:

```bash
source "$HOME/.profile"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` configures the project with CMake, builds it, and runs the
tests. The generated `.deb` is placed in the parent directory of the project,
for example:

```text
../onedrive-cpp_0.5.0-1~ubuntu24.04_amd64.deb
```

The Debian changelog carries the native build distribution suffix. When
adding Ubuntu 26.04 support, build from an Ubuntu 26.04 environment with a
`0.5.0-1~ubuntu26.04` changelog version.

Install and verify the package:

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
man onedrive-cpp
```

To check for Debian policy issues:

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```

## Configuration and systemd

By default, the application first reads `~/.config/onedrive-cpp/config.toml`. If
that file does not exist, built-in defaults are used. A system-wide example is
installed at `/etc/onedrive-cpp/onedrive-cpp.toml`. To create a user
configuration:

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

Configuration files use TOML and must declare `config_version = 1`. Unknown
keys and invalid value types are rejected instead of being silently ignored.

Before running a command, the client secures `state.directory` to owner-only
`0700`, validates the active account token path, and acquires an exclusive
`onedrive-cpp.lock` in that directory. A second process using the same state
directory fails immediately. Existing private state files are tightened to
`0600` when owned by the current user; symbolic links and files owned by
another user are rejected.

For synchronization, `sync.directory` and `state.directory` must not contain
one another, the filesystem root cannot be used as the sync directory, and
every existing path component must be free of symbolic links. Normal sync
performs a create/write/fsync/remove probe before contacting Graph. Dry-run
keeps its non-mutating sync-directory behavior. Downloads also reserve the
larger of 256 MiB or 5% of the planned transfer in addition to the download
bytes.

`sync.drive_id` selects the remote OneDrive drive to access. The default value,
`me`, selects the signed-in user's default OneDrive and lists its root through
the Microsoft Graph path `/me/drive/root/children`. To access another OneDrive
or a SharePoint document library available to the account, set the actual
Drive ID instead; the client then uses `/drives/<drive_id>/root/children`.
For example:

```toml
[sync]
# Default OneDrive of the signed-in user
drive_id = "me"
download_concurrency = 4
download_chunk_threshold_bytes = 8388608

# Another OneDrive or SharePoint document library
# drive_id = "b!YOUR_DRIVE_ID"
```

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

`download_concurrency` controls how many files can be downloaded at the same
time. It defaults to `4` and accepts values from `1` through `16`.

`download_chunk_threshold_bytes` sets the large-file threshold in bytes. Files
larger than this value are downloaded sequentially with HTTP byte-range
requests, using the same value as the maximum chunk size. It defaults to
`8388608` (8 MiB) and must be greater than zero. Files at or below the
threshold use a single request.

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

### Microsoft authentication

The client uses the OAuth 2.0 Device Authorization Grant. You must register
your own public client application in Microsoft Entra; do not create a client
secret.

#### Register the application

Microsoft currently requires an Azure account with an active subscription,
access to a Microsoft Entra tenant, and permission to register applications.
If you only have a personal Outlook.com/Hotmail account and cannot open
**App registrations**, create a
[free Azure account](https://azure.microsoft.com/pricing/purchase-options/azure-account)
and use its **Default Directory**, or ask the tenant administrator for the
Application Developer role.

1. Sign in to the
   [Microsoft Entra admin center](https://entra.microsoft.com/).
2. Open **Entra ID > App registrations > New registration**.
3. Enter a name such as `onedrive-cpp`.
4. Select the supported account type:
   - For both work/school and personal Microsoft accounts, select
     **Any Entra ID Tenant + Personal Microsoft accounts** and later use
     `auth.tenant_id = "common"`.
   - For personal Microsoft accounts only, select **Personal accounts only**
     and later use `auth.tenant_id = "consumers"`.
   - For one organization only, select **Single tenant**, then use that
     directory's tenant ID instead of `common`.
5. Select **Register**.
6. On the application **Overview** page, copy the **Application (client) ID**.
   Do not copy the Object ID or Directory ID into `auth.application_id`.
7. Open **Authentication > Advanced settings**, set
   **Allow public client flows** to **Yes**, and save.

Device code flow does not require a redirect URI. Because this is a public
client, do not add a client secret: a secret embedded in a desktop or CLI
application cannot be kept confidential.

Microsoft's corresponding documentation is:

- [Register an application in Microsoft Entra ID](https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app)
- [Configure desktop and public client applications](https://learn.microsoft.com/en-us/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 device authorization grant](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)

#### Configure permissions

Open **API permissions > Add a permission > Microsoft Graph > Delegated
permissions**.

For a personal OneDrive account, configure these delegated permissions:

```text
User.Read
Files.ReadWrite
```

`User.Read` allows the client to identify the signed-in account and download
its profile photo. The client also requests `offline_access` so Microsoft can
issue a refresh token. For organizational OneDrive, shared libraries, and SharePoint
scenarios, add the broader delegated permissions only when required:

```text
Files.ReadWrite.All
Sites.ReadWrite.All
```

Organizational tenant policies may require an administrator to grant consent.
Personal Microsoft accounts normally grant consent during device sign-in.
See the
[Microsoft Graph permissions reference](https://learn.microsoft.com/en-us/graph/permissions-reference)
for the current permission definitions.

#### Configure onedrive-cpp

Create the user configuration if it does not already exist:

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

For an application registered as **Personal Microsoft accounts only**, use:

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "consumers"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

For an application registered as **Any Entra ID Tenant + Personal Microsoft
accounts**, use `common` even when the user signing in has a personal account:

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "common"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

For a single-tenant organizational application, replace `common` with the
tenant's Directory (tenant) ID. Add `Files.ReadWrite.All` or
`Sites.ReadWrite.All` only for organizational scenarios that require them;
`Sites.ReadWrite.All` is not supported for personal Microsoft accounts.

#### Authorize the client

Run the device authorization flow:

```bash
onedrive-cpp auth
```

Open the displayed URL, enter the user code, sign in with the account matching
the registration's supported account type, and review the requested
permissions. On success, the client retrieves the stable user and Drive
identities plus the profile photo. The refresh token and photo are atomically
saved under the friendly account state directory with owner-only permissions.

If Microsoft reports that the account is unsupported, verify the selected
**Supported account types** and use `consumers` for personal-only
registrations or `common` for combined personal and organizational
registrations.

If the program displays `https://www.microsoft.com/link` and that page
immediately reports that a newly generated code is invalid or expired, check
whether a combined personal and organizational application was incorrectly
configured with `auth.tenant_id = "consumers"`. Change it to:

```toml
[auth]
tenant_id = "common"
```

Start `onedrive-cpp auth` again and use only the newly generated code at the
newly displayed `https://login.microsoft.com/device` URL. Previously generated
device codes cannot be reused. Keep `consumers` only for applications whose
supported account type is **Personal Microsoft accounts only**.

If the device code is accepted and account sign-in begins, but Microsoft then
reports that the code expired while the terminal remains at
`Waiting for authorization...`, remove permissions that are unavailable to
the selected account type. In particular, personal Microsoft accounts should
use:

```toml
[auth]
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

After changing scopes, restart `onedrive-cpp auth`; an existing device code
retains its original scopes and cannot be repaired or reused.

Remove the saved authentication with:

```bash
onedrive-cpp logout
```

Reset the saved `deltaLink` for the configured drive with:

```bash
onedrive-cpp reset-state
```

This preserves authentication tokens, configuration, local files, item
snapshots, pending-download recovery records, and state for other drives. The
next `sync` recovers pending downloads first, then performs a full initial
Delta query. Existing snapshots are used to detect local modifications safely;
blocked-item records are preserved for diagnosis, and the complete remote
inventory replaces the configured Drive's old item metadata.

During a Delta query, text output and logs report each completed page and the
cumulative number of scanned items. JSON output emits a `delta_progress` event
with `pages`, `items`, and `completed` fields. Microsoft Graph does not provide
the total number of Delta items in advance, so an accurate percentage is not
available. `--quiet` suppresses console progress but not configured log output.

Before creating directories or downloading files, synchronization validates
every remote path. Empty, absolute, dot-segment, NUL-containing, control-byte,
and empty components are rejected. Component and complete-path byte lengths
are checked against the target filesystem limits. The error identifies the
remote path, offending component, actual size, and supported limit; control
bytes are escaped so diagnostics remain safe to display. Invalid names are
persisted as blocked items while independent files continue synchronizing.
They are retried automatically and normally require renaming in OneDrive.

To discard all saved synchronization state for the configured Drive, use the
explicitly destructive mode:

```bash
onedrive-cpp reset-state --clear-all
```

The command requires the configured Drive reference (for example, `me`) to be
typed exactly before it removes item snapshots, the Delta cursor,
pending-download recovery records, and blocked items. If no configured
reference is available, it requires the raw Drive ID instead. Local files and
other Drives remain untouched. Because local snapshots are no longer
available, the next sync may report local modification conflicts. Automation
must acknowledge this risk explicitly with
`reset-state --clear-all --yes`; `--yes` is rejected without `--clear-all`.

The `sync` command refreshes the OAuth access token, securely persists a
rotated refresh token when Microsoft returns one, and obtains the configured
drive's recursive file tree through paginated Microsoft Graph delta requests.
The first successful query atomically writes remote metadata and the final
`deltaLink` to the selected account and Drive's `items.sqlite3`; later runs reuse that link
and retrieve only added, changed, and deleted items. The `deltaLink` advances
only after every page has been processed successfully, so a partial failure
does not lose unapplied changes.

`--dry-run` queries remote changes and reports directory creations, file
downloads, download bytes, and local removals, but does not create files or
update SQLite state. Normal mode creates remote directories and downloads
added or changed files. Each download is written to a temporary file in the
target directory, size-checked, flushed, and atomically replaced. Each
concurrent worker commits its completed file immediately instead of retaining
the entire batch as temporary files, so a later independent failure does not
discard completed transfers. The `deltaLink` advances after every change is
either applied successfully or durably recorded as blocked in the same SQLite
transaction. Local snapshots for completed files support safe retries after a
systemic failure.

Each download holds an operation-coordinator lease keyed by
`(drive_id, remote_id)` from preparation through the final ItemStore update.
Operations for the same remote item are serialized while different items and
drives remain concurrent. Future upload, delete, and rename paths must acquire
the same lease so their network, filesystem, and state transitions cannot
overlap for one item.

The client refuses to overwrite a local file that it cannot prove is
unchanged. Before downloading, it captures the destination's existence, size,
modification time, and SHA-256 fingerprint, then checks that baseline before
journaling and immediately before the atomic replacement. A file created or
modified during transfer is retained and persisted as a `local_modification`
blocked item. Invalid remote paths, symbolic links, local path-type conflicts,
and descendants of blocked directories are also persisted as blocked items.
Independent files continue, and `sync` exits with status 2 after safely
advancing the cursor. Blocked items are retried on every later incremental
sync; a successful retry or remote deletion clears the record.
Authentication, Graph, database, root-permission, disk-capacity, and download
transport failures remain fatal. The client does not yet upload local changes
or remove local files for remote deletion records.

After a download completes, the client writes the temporary path, destination,
remote metadata, size, and SHA-256 content fingerprint to a SQLite
`pending_download` journal before the atomic replacement. Startup recovery
processes this journal first, making SQLite authoritative rather than relying
on target-file-system extended attributes.

Large downloads also persist a separate SQLite `partial_download` checkpoint
after each successful, flushed Range chunk. The checkpoint records the remote
ETag, expected size, destination, temporary path, and durable byte count. A
later process resumes only when that metadata and the same-directory regular
file still match; uncheckpointed trailing bytes are truncated, while stale or
changed remote versions restart from byte zero. The completed download moves
from partial state to the existing pending-install journal before replacement.

Download progress is aggregated across concurrent transfers. Text output shows
completed and total file counts, overall byte percentage, and transferred size
using B, KiB, MiB, or GiB. Interactive terminals update one concise line in
place; redirected text and JSON output receive progress events at
one-percentage-point intervals. JSON events retain exact byte counts and include
`completed_files`, `file_count`, `downloaded_bytes`, `total_bytes`,
`percentage`, and `completed`. `--quiet` suppresses progress output.

If Microsoft Graph rejects a saved Delta cursor with `410 Gone`, synchronization
automatically retries with a full Delta query. Existing local snapshots remain
available for conflict detection, and the saved cursor and remote inventory are
replaced only after the full synchronization plan succeeds.

`filesystem.metadata` controls optional `user.*` xattr hints:

```toml
[filesystem]
metadata = "auto"
```

- `auto` verifies xattr behavior with an actual probe file, uses hints when
  supported, and automatically falls back to the database journal otherwise.
- `xattr` requires xattr support and stops synchronization if the probe fails.
- `database` never reads or writes xattrs and is suitable for FUSE, SMB/NFS,
  and file systems with unreliable extended-attribute semantics.

Capability detection probes the target synchronization directory instead of
using a file-system-name allowlist.

Microsoft Graph page requests, download redirects, file downloads, and ranged
chunks retry HTTP 429, 502, 503, and 504 responses. The client honors a numeric
`Retry-After` header and otherwise uses bounded exponential backoff. Failed
ranged requests roll the temporary file back to the chunk boundary before
retrying. Retries are limited so persistent service failures fail explicitly
instead of waiting forever.

The throttling policy can be adjusted in the configuration file:

```toml
[graph.throttle]
maximum_retries = 4
initial_delay_seconds = 1
maximum_delay_seconds = 300
```

The initial delay is doubled after each retryable response without a valid
numeric `Retry-After`, up to the configured maximum. A server-provided delay
above the configured maximum is rejected instead of sleeping unexpectedly long.

After installing the DEB package, enable the user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

The service currently runs the monitor scaffold. It does not access OneDrive
or modify the synchronization directory.

## Suggested Next Steps

1. Safely apply remote deletions and moves to the local file tree.
2. Add uploads and two-way conflict policies.
3. Connect the monitor to inotify and add integration tests for the Graph and
   file system boundaries.

## License

GPL-3.0-or-later. The reference project also uses GPLv3, which makes this
license suitable for future reuse or adaptation of its design in compliance
with the license terms.

## Legal and Branding

- [Terms of Service](TERMS.md)
- [Privacy Statement](PRIVACY.md)
- Project logo: [SVG](assets/onedrive-cpp-logo.svg) |
  [512x512 PNG](assets/onedrive-cpp-logo.png)

The legal documents are project-maintainer drafts, not legal advice. Review
them for the applicable organization and jurisdiction before a public or
commercial release. onedrive-cpp is an independent project and is not
affiliated with or endorsed by Microsoft.
