# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client for
Ubuntu 24.04 and 26.04 LTS. For Ubuntu 26.04, it has gui support. Its separation of responsibilities is inspired by
[abraunegg/onedrive](https://github.com/abraunegg/onedrive), but it does not
copy the reference project's D implementation.

The current development version is **1.2.7**. CLI, TUI, GUI, user-agent and CPack versions
are generated from `version-semver` in `vcpkg.json`; Debian source packaging
records its distribution-specific version separately in `debian/changelog`.

## Features

- [OAuth 2.0 authentication](docs/authentication.md): GUI and desktop CLI/TUI
  system-browser Authorization Code + PKCE (S256) login with a loopback
  callback, plus automatic or explicit (`--auth-flow`) CLI device-code login;
  shared per-account credentials, automatic access-token refresh, and
  owner-only refresh-token storage.
- Incremental Microsoft Graph Delta synchronization with multiple account and
  Drive isolation, selective-sync rules, explicit single-file downloads, and
  missing-local-file recovery with `transfer restore`.
- Concurrent, resumable, integrity-checked downloads with atomic installation,
  disk-space coordination, aggregate rate limiting, and local-conflict backup
  or blocking policies.
- Concurrent simple and upload-session transfers with durable snapshots,
  checkpoints, recovery, per-file and aggregate rate limits, and conditional
  Graph writes.
- Bidirectional file and directory creation, modification, deletion, move, and
  rename propagation, including dependency-cycle staging and configurable
  large-deletion protection.
- SQLite-backed crash recovery for downloads, uploads, local and remote moves,
  remote deletions, and directory creation.
- Long-running inotify monitoring with Graph WebSocket notifications, polling
  fallback, event coalescing, overflow recovery, and clean signal handling.
- Optional English/Simplified Chinese Qt GUI with live language switching,
  validated settings editing/import, unsaved-change indicators and close
  confirmation, SharePoint library discovery, shared Monitor ownership and
  controls, persistent blocked-item inspection, and session/round statistics.
- Completed file-operation history in CLI and GUI with timestamps, actions,
  old/new paths, configurable retention, and explicit omission counts.
- English and Simplified Chinese Console and interactive FTXUI terminal
  interfaces (`--language`, `ONEDRIVE_CPP_LANG`, `console.language`), dry-run
  and structured JSON output, rotating logs, proxy support, shell completion,
  bilingual manual pages, a hardened systemd user service, and Debian packaging.

OneNote files and packages recognized from Graph metadata are not supported.
They are explicitly reported and skipped while other files continue; existing
local copies are preserved and the sync exits with status 3.
See [unsupported formats](docs/synchronization.md#unsupported-formats).

## Documentation

The [documentation guide](docs/README.md) organizes the manuals into three
chapters:

1. **Getting started:** [authentication](docs/authentication.md),
   [configuration](docs/configuration.md), and
   [command usage](docs/usage.md).
2. **Synchronization operations:**
   [synchronization and recovery](docs/synchronization.md), selection rules,
   conflict policies, monitoring, diagnostics, and database repair.
3. **Design and development:** [architecture](docs/architecture.md),
   [development and testing](docs/development.md), and
   [license and project information](docs/project.md).

## Requirements

### Supported platforms

- Ubuntu 24.04 LTS and later, including Ubuntu 26.04 LTS.
- x86_64 or arm64, depending on the LLVM and Ubuntu build environment.
- Clang 20 with `-std=c++2c` / CMake `CXX_STANDARD 26`.
- CMake 3.28 or later.
- Development packages for CLI11, libcurl/OpenSSL, Microsoft GSL,
  nlohmann/json, spdlog/fmt, SQLite, and toml++ from the system package
  manager.

The checked-in CMake presets select `clang++-20` and Ninja explicitly.
Installing only `build-essential` or the distribution's default compiler does
not satisfy these presets.

### Build environment

Install the build tools from the Ubuntu repositories on 24.04 or 26.04:

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 clang-format-20 clang-tidy-20 \
  zip unzip tar pkg-config \
  libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

Verify the installed versions:

```bash
clang++-20 --version
clang-format-20 --version
clang-tidy-20 --version
cmake --version
ninja --version
```

#### Ubuntu 26.04 development requirements

- Enable the official `universe` repository before installing the packages
  above; it supplies Ninja, Clang 20, and several development libraries.
  Package installation requires administrator (`sudo`) access.
- Use the versioned Clang 20 compiler, formatter, and lint tools even if a
  different compiler is already installed. The Ubuntu 26.04 CMake 4.2 and
  Ninja 1.13 packages meet the project's build-tool requirements; no CMake
  preset changes are needed.
- Keep `build-essential` installed alongside Clang for the system C++ standard
  library headers and build tools.
- Allow HTTPS access to GitHub during the first CMake configuration so
  FetchContent can download the pinned FTXUI 7.0.3 and Proxy 4.1.0 dependencies.
  Ubuntu 26.04's `libftxui-dev` 5.0 package is too old; CMake rejects it and
  uses the pinned source instead.

After installation, use the Debug or Release build and CTest commands below.
Installing packages alone does not verify that the project builds or its tests
pass on the machine.

CMake checks system fmt/spdlog with the selected compiler. When Clang encounters
the known fmt 9/10 compile-time format-string bug, it applies a small backport of
the [upstream fix](https://github.com/fmtlib/fmt/commit/6797f0c) to a build-local
copy of the installed headers. System headers are not modified, and the program
still links to system fmt/spdlog shared libraries. Configuration verifies that
valid format strings compile and invalid ones are rejected; it stops if the
patch cannot be applied safely. Once the system headers pass the check, the
compatibility copy is no longer used.

Format modified C or C++ lines before committing. Stage the source/header
changes, then run the versioned Git integration from the repository root so it
uses the checked-in `.clang-format` without reformatting unrelated legacy
lines:

```bash
git add path/to/modified.cpp path/to/modified.hpp
git-clang-format-20 --binary clang-format-20 --staged
git add path/to/modified.cpp path/to/modified.hpp
```

New files may instead be formatted in full with `clang-format-20 -i`. Do not
use an unversioned formatter or a different major version, because its output
may differ from Clang 20.

The compiled libraries are resolved from the operating system and linked
dynamically. This lets Debian security updates replace libcurl, OpenSSL,
SQLite, spdlog, and fmt without rebuilding `onedrive-cpp`.
Development builds accept FTXUI 7.0.3 or later in the 7.x series and Proxy
4.1 or later in the 4.x series as installed CMake packages. Otherwise CMake
uses the fixed source archives recorded in `packaging/dependencies.json`,
downloading them when necessary. Proxy 4 is header-only. Release builds use
the verified bundled archives instead of system copies of these two dependencies.
Bundled-source licenses are installed under `share/doc/onedrive-cpp/licenses`.
The vcpkg manifest resolves these dependencies through the `ftxui` and `proxy` ports.

## Build

After cloning the repository, change to its root directory. All build commands
below assume the project root is the current working directory.

### Optional desktop GUI

The **About** button at the right of the page tabs shows the build version,
license, and GitHub project link. There is no separate menu bar.

The GUI requires Qt 6.8 or later, including Linguist tools and Qt's Simplified
Chinese translation catalog. On Ubuntu 26.04:

```bash
sudo apt install -y qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-translations-l10n
cmake --preset release -DONEDRIVE_BUILD_GUI=ON
cmake --build --preset release --target onedrive-cpp-gui
./build/release/onedrive-cpp-gui
# Optional startup language override (no language-selection dialog):
./build/release/onedrive-cpp-gui --language en
./build/release/onedrive-cpp-gui --language=zh_CN
./build/release/onedrive-cpp-gui --language auto
```

By default (`--language auto`), the GUI follows the system locale at startup:
Simplified Chinese locales
display **OneDrive 客户端**, and English or unsupported locales display
**onedrive client**. The overview, settings, login statuses, dialogs, and
browser callback messages are translated. Application and Qt translations are
embedded in the executable; no separate translation files are needed at runtime.
The exact accepted `--language` values are `auto`, `en`, and `zh_CN`; unsupported
values or unknown options exit with an error. `--help` and `--version` print
information without opening a window.

Use the **Language** dropdown at the right of the page tabs to switch between
**System language**, **English**, and
**Simplified Chinese** immediately. Existing labels, status messages, settings,
and the window title update without losing unsaved settings drafts or restarting
an active login or synchronization task. The dropdown shows the current selection.
Both command-line and dropdown selections apply only to the current session and
are not saved.
Language selection and **About** remain available during login, synchronization,
and monitoring, even when Settings is disabled.
Restart after changing the system language. Native dialogs and the external
browser may follow their own system settings.

Underlying configuration, filesystem, backend, and server error details are
preserved and may still be in English; their GUI context is translated.
Startup is local-only: it never checks credentials online, opens a browser, or
prompts for login. Missing configuration uses defaults with a setup notice
(save **Application ID** in **Settings → Account and directories** before signing
in). Invalid configuration or
unreadable account state keeps Settings available and disables sign-in until
repaired. Import a valid TOML file, or edit the active file and use **Reload
configuration**. Invalid files are not silently overwritten; imports preserve
their contents in a backup. Startup reports whether saved sign-in information
was found locally; online operations check authentication, refresh credentials
when needed, and report authentication or connectivity errors.
Sync-path validation does not prevent login when authentication/state settings
are usable; synchronization requires valid sync settings and saved credentials.

The main tabs are **Overview / Sync / Files / Settings / Logs**. Overview
shows account and directory information plus the current/latest sync result.
Overview has no synchronization buttons; task controls are on the Sync tab.

In **Synchronization status**, use **Preview changes** to inspect the plan
without changing synchronized files, then **Sync now** to apply it. Both use
saved configuration, not unsaved Settings drafts. A saved `sync.dry_run = true`
keeps **Sync now** in preview mode; the GUI never overrides the remote-deletion
safety limit. Tasks run in a background worker using the same service and state
directory lock as the CLI. The task page shows a concise result/error summary
with details available in the **Logs** tab. The page shows the configured direction
and one-time task mode, separate upload/download counts, bytes, speed and an
estimated remaining time when available. Blue progress bars display centered
numeric percentages (for example, `50%`). Transfer percentages measure their own
direction, not the whole task; uploading 100% does not complete synchronization.
Saved resume offsets and reused downloads do not inflate current-round network
speed. The round summary retains download/upload/move/delete/block counts and the
last finish time.
Scanning and other phases without measurable totals display an explanatory
message instead of a full-looking indeterminate bar. Overall 100% appears only
after successful task completion.
**Stop** requests cancellation and waits for cleanup. Login, logout and settings
changes are disabled during a task; quitting the application cancels and joins it.

When a system tray is available, closing the window hides it while synchronization
continues. Left-click the tray icon to open/raise the window; its context menu
always offers **Open window**, **Open sync folder**, and **Quit**. Closing the
window or choosing **Quit** confirms unsaved settings; **Quit** also waits for
active login/synchronization cleanup. Without a usable tray, closing the window
exits as before. If the tray disappears while the window is hidden, the window
reopens so the application remains accessible.

**Start automatically at login** is available in Settings and the tray menu.
Both switches apply immediately and share the user desktop autostart entry at
`$XDG_CONFIG_HOME/autostart/onedrive-cpp-gui.desktop` (normally
`~/.config/autostart/onedrive-cpp-gui.desktop`), independently of TOML settings.
On desktop login, the GUI starts in the tray without opening its window; without
a usable tray it shows the window. Manual launches still show the window.
This starts the GUI, not a synchronization task.

The **Blocked items** list below synchronization status reads the active Drive's
saved database without contacting Graph. Preview, synchronization and CLI Monitor
discoveries share this list; there is no separate preview-only list. Search paths
or reasons, filter by reason code, and select a row to view/copy its full details.
Refresh rereads saved records. Clearing logs does not clear blockers, and records
survive task failure, cancellation and application restarts. A later validated
resolution removes the corresponding record; merely not seeing it does not.
The CLI reads the same records offline with `onedrive-cpp inspect blocked`;
`--reason CODE` and `--search TEXT` filter results, and `--output json` supports
automation. See [usage](docs/usage.md) for fields and TUI controls.
Preview may create/update blocked diagnostics, but never transfers files,
advances the applied cursor or commits normal file snapshots.

Blocked-item details also explain the shared engine strategy. OneNote and other
unsupported-format restrictions are fixed and cannot be changed. Other reasons
follow their own revalidation rules and existing conflict/deletion settings;
the viewer does not provide a generic unblock/override action.

The GUI retains complete preview metadata in memory: the next preview/sync
checks only changes since that preview, merges them with unapplied changes,
and rebuilds the plan against current local files. Preview never advances the
applied cursor. Configuration, account, filter or persisted-state changes
invalidate the cache; expired cursors and directory moves/deletions fall back
to a full query. Pending recovery prevents caching. Cancellation/failure clears
it, and apply consumes it; nothing is saved across GUI restarts.

Actual synchronization in both CLI and GUI saves a separate **pending remote
delta batch** in SQLite before executing the plan. After interruption/restart,
the next sync restores those raw changes, queries only changes since their
fetched cursor, and rebuilds the plan against current local state. Completed
files and partial downloads continue using existing recovery. The applied
cursor advances only when the remote plan is committed, atomically clearing the
pending batch. Changed account/root/Graph/auth/validation context, filters or
applied baseline invalidate reuse; expired cursors and directory hierarchy
changes fall back to a full query. Preview does not write pending batches.
`state reset` also discards pending delta metadata, without deleting file
recovery records.

**Logs** retains up to 500 application events across tasks for the current GUI
session, with timestamps, operation names and levels; omissions are explicitly
shown. The level filter is a minimum severity: Warning shows Warning, Error and
Critical; All levels shows everything. The English synchronization log tag is
`Sync`. With auto-scroll disabled, incoming events preserve the visible entry,
including when older history is discarded; if that entry expires, the view stays
at the oldest retained logs. Re-enabling auto-scroll jumps to the latest entry.
Copy the displayed logs or clear history without stopping a task. Tab/language switches preserve tasks, filters
and history. Backend logs are included according to the configured logging level,
including each download's file path and size. Frontend and backend share the
same six levels: Trace, Debug, Info, Warning, Error and Critical. Successful
application events use Info, not a separate Success level. Filtering does not
enable logs below the configured logging threshold. Raw backend details remain
unchanged; authorization URLs and tokens
are not added to account logs. This tab does not read the configured log file
or persist an additional log.
**Start monitoring** uses the shared Monitor worker with saved settings. While
monitoring, **Sync now** requests that Monitor rather than launching a second
engine. The GUI also discovers and can trigger/stop an external CLI or service
Monitor; stopping it requires confirmation. Quitting the GUI stops only its own
task, not an external Monitor. Direct systemd unit start/restart controls are not
provided. See [Monitor ownership and controls](docs/usage.md#gui-monitoring-and-external-ownership).

GUI-owned monitoring shows both session and last-round totals. Reported
download/upload/move/delete round counts accumulate; blocked items use
the latest round's count. Starting a new Monitor task resets session totals.
External Monitor snapshots do not expose transfer progress or these totals.

**Files** lists completed operations since application startup, with local
completion times, actions, paths, previous paths and Drive IDs. Search filters
the list. Clearing logs or restarting Monitor does not clear it; application
exit does. The shared `history.maximum_entries` setting defaults to **10000**,
retains the latest entries and displays the omitted count. Configure it under
**Settings / Advanced and logs**. Only current-process completions are listed,
not external Monitor history, previews, blocked attempts or already-current
reuse. CLI text/JSON streams completions; TUI shows recent entries and exports
the retained list when it ends. See [file-operation history](docs/usage.md#file-operation-history).

Hover over any setting, its label or browse/override control for a concise tip,
including advanced fields. Tips also provide accessibility descriptions and
switch languages immediately without changing the draft.

Settings phases one and two provide six sub-tabs: **Account and directories**,
**Sync rules**, **Transfers**, **Network**, **Monitor**, and **Advanced and logs**.
They share one draft and global save/import/
reload controls. Save validates with the core TOML parser, preserves comments
and untouched settings, backs up the original, and replaces the file atomically.
Import and reload replace the draft; switching tabs or language preserves it.
When any field or override differs from the loaded configuration, the window
title (`onedrive client *` / `OneDrive 客户端 *`) and Settings tab
(`Settings *` / `设置 *`) show a `" *"` indicator; saving, reloading, or
reverting edits clears it. Closing the window or quitting with
unsaved changes prompts to **Save**, **Discard**, or **Cancel**.
Directory changes do not migrate files or credentials. Changes apply to the next
operation; settings are disabled during login or synchronization.

The tabs cover Application/tenant/Drive IDs (including a **Browse SharePoint
libraries…** dialog) and directories; sync direction,
deletion/conflict/dotfile policies, selective rules, file-size limit and advanced
`.nosync`/root-file/deletion guard settings; upload/download concurrency,
per-request/aggregate rates, ordering, and advanced chunk/checkpoint/retry/
download validation settings. Sizes use bytes and rates bytes/s (`0` means
unlimited for sizes/rates, but zero remote deletions blocks deletion). Integers
retain TOML's full nonnegative signed 64-bit range.

Phase two adds proxy URL/bypass overrides, username and password/CA file
references, transport timeouts and HTTP/IP modes, Graph throttling, monitor
poll/settle/WebSocket settings, file-history retention, log level/file, file
permissions and metadata.
Unchecked proxy overrides inherit the environment independently; an explicitly
empty URL forces direct connections and an empty bypass list overrides `NO_PROXY`.
Passwords are never entered inline; referenced files are checked by runtime
security tools, not opened by the editor. Advanced account settings include
trusted HTTPS auth/Graph endpoints, scopes (changes require signing in again),
and a required mount point. No files, credentials or state are migrated.
Persistent dry-run/force-delete flags and CLI themes are not GUI settings.

Switching the GUI language affects only the running GUI window; CLI/TUI language
is configured separately via `--language`, `ONEDRIVE_CPP_LANG`, or
`console.language`, while JSON output, configuration keys, and log files remain
unchanged.
`ONEDRIVE_BUILD_GUI` defaults to `OFF`; headless builds need neither Qt nor
translation tools. See [GUI localization tests](docs/development.md#gui-localization)
for maintenance and offscreen validation.

### Release build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The executable is generated at `build/release/onedrive-cpp`. Verify it with:

```bash
./build/release/onedrive-cpp --version
./build/release/onedrive-cpp transfer sync --dry-run
./build/release/onedrive-cpp --help
```

### Clean Release rebuild

Delete only the Release build tree to force CMake and Ninja to reconfigure and
rebuild it from scratch:

```bash
rm -rf build/release
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/onedrive-cpp transfer sync --dry-run
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

## Installation

### Install from a CMake build

Install the Release build into the configured prefix:

```bash
sudo cmake --install build/release
```

The default prefix installs the executable, example configuration, systemd
user service, English and Simplified Chinese manual pages, and shell completion
files under `/usr/local`. Set `CMAKE_INSTALL_PREFIX` while configuring when a
different prefix is required.

The user service is generated at installation time, so its `ExecStart` follows
the actual prefix, including `cmake --install ... --prefix` overrides and CPack's
default `/usr` prefix. A `DESTDIR` staging directory is not embedded in the service.
For a custom prefix outside systemd's user unit search path, link the installed
unit as your regular user, then reload the user manager:

```bash
systemctl --user link /opt/onedrive-cpp/lib/systemd/user/onedrive-cpp.service
systemctl --user daemon-reload
```

When `ONEDRIVE_BUILD_GUI=ON`, installation also includes `onedrive-cpp-gui`,
a desktop application entry, and hicolor application icons. The CLI and GUI
are installed together; no separate GUI package is produced.

### Quick CPack package

Use CPack to quickly create a package that has not undergone full Debian
policy checks. On Ubuntu 24.04, build the CLI/TUI package:

```bash
cmake --preset release -DONEDRIVE_BUILD_GUI=OFF
cmake --build --preset release
ctest --preset release
```

On Ubuntu 26.04, install the Qt dependencies listed under
[Optional desktop GUI](#optional-desktop-gui), then build the combined
CLI/TUI and GUI package:

```bash
cmake --preset release -DONEDRIVE_BUILD_GUI=ON
cmake --build --preset release
ctest --preset release
```

After the appropriate build, create the package:

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

The generated package is placed in the current directory. CMake reads
`ID` and `VERSION_ID` from `/etc/os-release`, so an Ubuntu 24.04 amd64 build
is named:

```text
onedrive-cpp_<version>-<revision>~ubuntu24.04_amd64.deb
```

For a cross-distribution build environment, override the detected suffix
explicitly during configuration:

```bash
cmake --preset release \
  -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04
```

Packages for Ubuntu 26.04 must still be built and tested in an Ubuntu 26.04
environment; changing the suffix does not change the binary ABI.

The package revision defaults to the latest `debian/changelog` entry, without
its distribution suffix. To increment a CPack-only revision explicitly:

```bash
cmake --preset release -DONEDRIVE_PACKAGE_REVISION=3
```

Set it back to an empty string to follow the changelog again. Application
versions in CMake, `vcpkg.json`, and the changelog must agree. CPack rechecks
the source versions before packaging and rejects stale default revisions or
inconsistent version overrides; reconfigure and rebuild after changing release metadata.
The version scheme is `<application>-<revision>~<distribution>`, without epochs.

The initial release targets amd64 on both distributions. The Ubuntu 26.04
package includes Qt runtime dependencies and platform plugins; the Ubuntu
24.04 package has no GUI or Qt requirement. arm64 release validation is
not included in this scope.

The system configuration is registered as a Debian conffile (by default,
`/etc/onedrive-cpp/onedrive-cpp.toml`). Package upgrades preserve local changes
or ask how to resolve a changed packaged default; ordinary removal keeps the
configuration, while `apt purge onedrive-cpp` removes it. For unattended
upgrades that must keep local configuration, explicitly select dpkg's
`--force-confold` policy, including when upgrading from an older CPack package
that did not register this conffile. Direct `cmake --install` does not provide
dpkg's configuration protection.

### Prepare release source for offline builds

From a Git checkout, prepare the release source archive while online:

```bash
python3 tools/prepare_source.py
tar -xf build/source/onedrive-cpp-1.2.7.tar.xz -C build/source
cd build/source/onedrive-cpp-1.2.7
```

The archive contains the working-tree contents of Git-tracked files and the
SHA256-verified FTXUI/Proxy source archives under `third_party/`. Untracked
files and build caches are excluded; publish from a clean, committed checkout.
Third-party sources are not committed to Git. Archives are cached under
`build/dependency-archives`; `--archives-dir PATH --offline` prepares source
without network access and fails if a cached archive is missing or corrupt.
Archive timestamps use `SOURCE_DATE_EPOCH`, or the latest Git commit time.

Install the target distribution's OS build dependencies before disconnecting.
Debian rules always enable `ONEDRIVE_OFFLINE_BUILD=ON`: missing/corrupt bundled
archives fail explicitly, with no network or installed FTXUI/Proxy fallback.
For manual CMake/CPack builds from prepared source, pass
`-DONEDRIVE_OFFLINE_BUILD=ON` at configuration time as well. The source archive
does not contain an OS toolchain or Qt; matching target environments are still
required, and this alone does not guarantee bit-identical binary packages.

### Official Debian package

Install the packaging dependencies:

```bash
sudo apt install -y build-essential devscripts debhelper cmake ninja-build python3 git \
  clang-20 libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

For Ubuntu 26.04, also install the GUI build dependencies:

```bash
sudo apt install -y qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-translations-l10n
```

From the prepared source directory above, build the Ubuntu 24.04 CLI/TUI package with the
`nogui` build profile. This omits Qt build and runtime dependencies:

```bash
source "$HOME/.profile"
chmod +x debian/rules
DEB_BUILD_PROFILES=nogui dpkg-buildpackage --build=binary --no-sign
```

On Ubuntu 26.04, build without the `nogui` profile to include both CLI/TUI
and GUI in the same `onedrive-cpp` package:

```bash
source "$HOME/.profile"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` configures the project with CMake, builds it, and runs the
tests. The generated `.deb` is placed in the parent directory of the project,
for example:

```text
../onedrive-cpp_1.2.7-1~ubuntu24.04_amd64.deb
```

Debian rules override debhelper's `FETCHCONTENT_FULLY_DISCONNECTED` setting so
CMake can extract bundled dependencies. `ONEDRIVE_OFFLINE_BUILD=ON` still
requires local archives with matching SHA256 hashes and never falls back to
network downloads.

Native packages use Clang 20 on both targets. Debian rules disable the
distribution's automatic LTO/fat-LTO flags to avoid the problematic Clang/GNU
archive and linker path; Release optimization and `hardening=+all` remain enabled.

Debian rules enable `ONEDRIVE_DEBIAN_PACKAGE_BUILD=ON`. Before dependency setup
or compilation, CMake requires the complete changelog version to match the
application, revision and actual build distribution, and the changelog suite
to match `/etc/os-release`'s `VERSION_CODENAME`. Selecting a build profile
does not change the version. The checked-in changelog targets Ubuntu 24.04
(`noble`); before building on Ubuntu 26.04 (`resolute`), update its entry, for example:

```bash
dch --newversion 1.2.7-1~ubuntu26.04 --distribution resolute
```

Use `noble` and the `ubuntu24.04` suffix for Ubuntu 24.04. Increment the Debian
revision for packaging-only changes; keep the application version unchanged.
Normal development and quick CPack builds allow a different changelog target
distribution, but still require the upstream application versions to agree.

Install and verify the package:

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
man onedrive-cpp
```

On Ubuntu 26.04, the application menu also provides **OneDrive Client**.
Run `onedrive-cpp-gui --version` to check the installed GUI version.

To check for Debian policy issues:

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```

### Package validation and release drafts

`.github/workflows/release.yml` builds native Debian packages on pull requests,
pushes to `main`, manual runs, and `v*` tags. It prepares one source archive with
verified dependencies, provisions separate Ubuntu 24.04 and 26.04 Docker images,
then builds and tests with networking disabled. Ubuntu 24.04 uses `nogui`;
Ubuntu 26.04 includes the GUI. No OneDrive credentials are required.
The test images restore manual-page installation and the real `man` executable
where the minimal base image disables them.

Buildx stores the dependency-image layers in GitHub Actions cache, separately
for each Ubuntu target and runner architecture, and checks for a newer base
image on each run. Compilation uses a separate persistent ccache (up to 1 GB
per target cache snapshot), keyed by target, architecture, build configuration
and commit, with compatible earlier snapshots restored for incremental reuse.
The compiler executable is checked by content; ordinary source and header
validation remains enabled. Native ccache builds disable precompiled headers
rather than relax ccache's macro/time checks; non-cached builds keep them.
A fixed `/work/source` path avoids changing the build directory with the
application version.

Only compiler results and dependency-image layers are reused: every run still
configures, links, tests and packages the current source, with the native build
and lifecycle containers offline. `.github/workflows/lint.yml` likewise uses a
persistent `ccache` cache (`ONEDRIVE_USE_PCH=OFF`) in its offline Ubuntu 26.04
Clang-Tidy container. The first run is a cold build; later gains
depend on cache hits and the size of the change. `ccache.txt` is uploaded with
the build logs, including on native-build failure. Delete the relevant Actions
caches to force a cold rebuild or refresh cached dependency installation layers.

The gates check application/package versions, run the CTest suite through
`dpkg-buildpackage`, and run Lintian (errors fail the job; warnings remain in
the uploaded report). Disposable target containers check package metadata,
licenses, CLI/GUI startup arguments, manual pages, desktop entry and service
syntax, configuration-preserving upgrades, removal, and purge.
The upgrade check uses a synthetic previous-package fixture with a changed
default configuration; it does not establish compatibility with every
previously published package. Real desktop interaction, systemd session startup,
and live account synchronization remain separate release checks.

Build logs, Lintian output, lifecycle logs, binary packages, and Debian build
metadata are retained as workflow artifacts. Only successful **tag pushes**
create a **draft** GitHub Release, with both distributions' packages, prepared
source, and `SHA256SUMS`. Publication remains a manual review step. A rerun will
not overwrite an existing release. The draft job alone receives `contents: write`;
build jobs use read-only repository permissions.

Use `v<application-version>` for Debian revision 1, and
`v<application-version>-<revision>` for subsequent packaging releases.
The current `1.2.7-1~ubuntu24.04` metadata requires tag **`v1.2.7`**; existing
release tags remain unchanged. The workflow rejects a tag that disagrees with
`vcpkg.json` or the changelog. After committing and pushing the release changes:

```bash
git tag -a v1.2.7 -m "onedrive-cpp 1.2.7"
git push origin v1.2.7
```

Enable GitHub Actions in the repository and allow workflows to create releases.
No personal access token is needed: the draft job uses the repository's
`GITHUB_TOKEN`. After both build jobs pass, review the artifacts and draft notes
before publishing. Use the attached prepared source archive for offline
dependency builds; GitHub's automatic source archives omit bundled dependencies.

For local reproduction, build the appropriate `.github/packaging.Dockerfile`
image and use the same `docker run --network none` commands as the workflow.
`tests/packaging/lifecycle.sh` installs and removes packages and requires root
plus `ONEDRIVE_DISPOSABLE_PACKAGE_TEST=1`; **never run it on your workstation**.
