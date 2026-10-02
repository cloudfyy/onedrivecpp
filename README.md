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
  and SQLite from the system package manager.

Ubuntu 24.04 provides the required CMake, Ninja, and Clang 20 packages through
its official repositories. GCC 13 is not used because its C++26 support is not
sufficient for this project configuration.

## Set Up the Build Environment

Install the build tools from the Ubuntu 24.04 repositories:

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 zip unzip tar pkg-config \
  libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libsqlite3-dev libssl-dev nlohmann-json3-dev
```

Verify the installed versions:

```bash
clang++-20 --version
cmake --version
```

The compiled libraries are resolved from the operating system and linked
dynamically. This lets Debian security updates replace libcurl, OpenSSL,
SQLite, spdlog, and fmt without rebuilding `onedrive-cpp`.

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

Every subcommand accepts `--log-level` and `--log-file`:

```bash
onedrive-cpp sync --dry-run --log-level debug
onedrive-cpp monitor --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
```

Supported levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`,
and `off`. A configured log file rotates at 5 MiB and retains three older
files. Authentication tokens, device codes, and authorization headers must
never be written to logs.

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

## Build DEB Packages

### Quick CPack package

Use CPack to quickly create a package that has not undergone full Debian
policy checks:

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

The generated package is placed in the current directory.

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
../onedrive-cpp_<version>_<architecture>.deb
```

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

By default, the application first reads `~/.config/onedrive-cpp/config`. If
that file does not exist, built-in defaults are used. A system-wide example is
installed at `/etc/onedrive-cpp/onedrive-cpp.conf`. To create a user
configuration:

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.conf ~/.config/onedrive-cpp/config
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config
```

`drive_id` selects the remote OneDrive drive to access. The default value,
`me`, selects the signed-in user's default OneDrive and lists its root through
the Microsoft Graph path `/me/drive/root/children`. To access another OneDrive
or a SharePoint document library available to the account, set the actual
Drive ID instead; the client then uses `/drives/<drive_id>/root/children`.
For example:

```ini
# Default OneDrive of the signed-in user
drive_id=me

# Another OneDrive or SharePoint document library
# drive_id=b!YOUR_DRIVE_ID
```

Tracked remote IDs, ETags, and local paths are stored in
`<state_directory>/items.sqlite3`. The database uses SQLite WAL mode and is
loaded when the application starts.

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
     `azure_tenant_id=common`.
   - For personal Microsoft accounts only, select **Personal accounts only**
     and later use `azure_tenant_id=consumers`.
   - For one organization only, select **Single tenant**, then use that
     directory's tenant ID instead of `common`.
5. Select **Register**.
6. On the application **Overview** page, copy the **Application (client) ID**.
   Do not copy the Object ID or Directory ID into `application_id`.
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

For a personal OneDrive account, start with the least-privilege delegated
permission:

```text
Files.ReadWrite
```

The client also requests `offline_access` so Microsoft can issue a refresh
token. For organizational OneDrive, shared libraries, and SharePoint
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
cp /etc/onedrive-cpp/onedrive-cpp.conf ~/.config/onedrive-cpp/config
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config
```

For an application registered as **Personal Microsoft accounts only**, use:

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=consumers
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite offline_access
```

For an application registered as **Any Entra ID Tenant + Personal Microsoft
accounts**, use `common` even when the user signing in has a personal account:

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=common
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite offline_access
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
permissions. On success, the refresh token is atomically saved to
`<state_directory>/refresh_token` with owner-only `0600` permissions.

If Microsoft reports that the account is unsupported, verify the selected
**Supported account types** and use `consumers` for personal-only
registrations or `common` for combined personal and organizational
registrations.

If the program displays `https://www.microsoft.com/link` and that page
immediately reports that a newly generated code is invalid or expired, check
whether a combined personal and organizational application was incorrectly
configured with `azure_tenant_id=consumers`. Change it to:

```ini
azure_tenant_id=common
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

```ini
auth_scope=Files.ReadWrite offline_access
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
after all planned operations succeed, the complete remote inventory replaces
the configured Drive's old item metadata.

To discard all saved synchronization state for the configured Drive, use the
explicitly destructive mode:

```bash
onedrive-cpp reset-state --clear-all
```

The command requires the configured Drive ID to be typed exactly before it
removes item snapshots, the Delta cursor, and pending-download recovery
records. Local files and other Drives remain untouched. Because local
snapshots are no longer available, the next sync may report local modification
conflicts. Automation must acknowledge this risk explicitly with
`reset-state --clear-all --yes`; `--yes` is rejected without `--clear-all`.

The `sync` command refreshes the OAuth access token, securely persists a
rotated refresh token when Microsoft returns one, and obtains the configured
drive's recursive file tree through paginated Microsoft Graph delta requests.
The first successful query atomically writes remote metadata and the final
`deltaLink` to `<state_directory>/items.sqlite3`; later runs reuse that link
and retrieve only added, changed, and deleted items. The `deltaLink` advances
only after every page has been processed successfully, so a partial failure
does not lose unapplied changes.

`--dry-run` queries remote changes and reports directory creations, file
downloads, download bytes, and local removals, but does not create files or
update SQLite state. Normal mode creates remote directories and downloads
added or changed files. Each download is written to a temporary file in the
target directory, size-checked, flushed, and atomically replaced. The
`deltaLink` advances only after every planned operation succeeds. Local
snapshots for completed files support safe retries after a partial failure.

The client refuses to overwrite a local file that it cannot prove is
unchanged, reports a `local modification conflict`, and stops. It does not yet
upload local changes or remove local files for remote deletion records.

After a download completes, the client writes the temporary path, destination,
remote metadata, size, and SHA-256 content fingerprint to a SQLite
`pending_download` journal before the atomic replacement. Startup recovery
processes this journal first, making SQLite authoritative rather than relying
on target-file-system extended attributes.

`filesystem_metadata` controls optional `user.*` xattr hints:

```ini
filesystem_metadata=auto
```

- `auto` verifies xattr behavior with an actual probe file, uses hints when
  supported, and automatically falls back to the database journal otherwise.
- `xattr` requires xattr support and stops synchronization if the probe fails.
- `database` never reads or writes xattrs and is suitable for FUSE, SMB/NFS,
  and file systems with unreliable extended-attribute semantics.

Capability detection probes the target synchronization directory instead of
using a file-system-name allowlist.

When Microsoft Graph returns HTTP 429, the client honors a numeric `Retry-After`
header and otherwise uses bounded exponential backoff. Retries are limited so
persistent throttling fails explicitly instead of waiting forever.

The throttling policy can be adjusted in the configuration file:

```ini
graph_maximum_throttle_retries=4
graph_initial_throttle_delay_seconds=1
graph_maximum_throttle_delay_seconds=300
filesystem_metadata=auto
```

The initial delay is doubled after each 429 response without a valid numeric
`Retry-After`, up to the configured maximum. A server-provided delay above
the configured maximum is rejected instead of sleeping unexpectedly long.

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
