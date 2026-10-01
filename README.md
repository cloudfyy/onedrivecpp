# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client scaffold for
Ubuntu 24.04 LTS and later. Its separation of responsibilities is inspired by
[abraunegg/onedrive](https://github.com/abraunegg/onedrive), but it does not
copy the reference project's D implementation.

> The current version provides a buildable architecture scaffold,
> configuration loading, a CLI, Microsoft device-code authentication, an HTTP
> transport layer, SQLite state persistence, dry-run support, a systemd user
> service, and Debian packaging. Microsoft Graph file operations and actual
> file synchronization have not yet been implemented. Non-dry-run
> synchronization exits with an explicit error.

## Architecture

```text
CLI / Application
       |
       +-- Config
       +-- DeviceAuth / TokenStore
       +-- HttpTransport (libcurl)
       +-- Monitor (file system event entry point)
       +-- SyncEngine (synchronization orchestration)
               |
               +-- GraphClient (Microsoft Graph boundary)
               +-- ItemDatabase (local state boundary)
```

The directories correspond to the responsibilities of
`main/config/curlEngine/onedrive/sync/itemdb/monitor` in the reference project:

- `src/app`: CLI parsing and application lifecycle.
- `src/auth`: Device-code OAuth, token refresh, and secure token persistence.
- `src/config`: Configuration file loading and validation.
- `src/graph`: Microsoft Graph API boundary.
- `src/http`: Typed libcurl HTTP transport.
- `src/storage`: SQLite-persisted remote ID, ETag, and local path state.
- `src/sync`: Difference calculation and synchronization orchestration.
- `src/monitor`: Long-running monitor mode entry point.
- `packaging/systemd`: systemd user service.
- `debian`: Native Ubuntu/Debian package metadata.

## Supported Platforms

- Ubuntu 24.04 LTS and later.
- x86_64 or arm64, depending on the LLVM and Ubuntu build environment.
- Clang 20 with `-std=c++2c` / CMake `CXX_STANDARD 26`.
- CMake 3.28 or later.
- vcpkg in manifest mode for C++ dependencies.

Ubuntu 24.04 provides the required CMake, Ninja, and Clang 20 packages through
its official repositories. GCC 13 is not used because its C++26 support is not
sufficient for this project configuration.

## Set Up the Build Environment

Install the build tools from the Ubuntu 24.04 repositories:

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 zip unzip tar pkg-config
```

Verify the installed versions:

```bash
clang++-20 --version
cmake --version
```

Install and bootstrap vcpkg:

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
```

Persist `VCPKG_ROOT` and load it in the current shell:

```bash
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> "$HOME/.profile"
source "$HOME/.profile"
```

The committed `vcpkg.json` manifest pins the registry baseline and is the
single source of truth for C++ library dependencies. It currently provides
libcurl, OpenSSL, nlohmann/json, and SQLite. vcpkg automatically installs
declared dependencies while CMake configures the project.

## Build and Test Locally

Run all build commands from the project root:

```bash
cd /home/yingying/onedrivecpp
source "$HOME/.profile"
```

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

### Clean Release rebuild

Delete only the Release build tree to force CMake, vcpkg, and Ninja to
reconfigure and rebuild it from scratch:

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

Each build tree has its own `vcpkg_installed` directory. Removing
`build/release` or `build/debug` therefore also forces vcpkg to restore that
configuration's dependencies during the next CMake configure step.

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

For a personal Microsoft account, use:

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=consumers
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite offline_access
```

To support both personal and work/school accounts with the same registration,
use `common`:

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=common
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite Files.ReadWrite.All Sites.ReadWrite.All offline_access
```

For a single-tenant organizational application, replace `common` with the
tenant's Directory (tenant) ID.

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

Remove the saved authentication with:

```bash
onedrive-cpp logout
```

The OAuth client implements refresh-token rotation, but Graph file operations
are not wired to the acquired token yet.

After installing the DEB package, enable the user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

The service currently runs the monitor scaffold. It does not access OneDrive
or modify the synchronization directory.

## Suggested Next Steps

1. Use the OAuth access token for authenticated Microsoft Graph drive and item
   requests.
2. Implement the delta API, conflict policies, and safe atomic file
   replacement.
3. Connect the monitor to inotify and add integration tests for the Graph and
   file system boundaries.

## License

GPL-3.0-or-later. The reference project also uses GPLv3, which makes this
license suitable for future reuse or adaptation of its design in compliance
with the license terms.
