# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client for
Ubuntu 24.04 LTS and later. Its separation of responsibilities is inspired by
[abraunegg/onedrive](https://github.com/abraunegg/onedrive), but it does not
copy the reference project's D implementation.

## Features

- OAuth 2.0 device-code authentication with secure per-account token and
  profile state.
- Incremental Microsoft Graph Delta synchronization with multiple account and
  Drive isolation, selective-sync rules, and explicit single-file downloads.
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
- Long-running inotify monitoring with Graph polling, event coalescing,
  overflow recovery, and clean signal handling.
- Dry-run and structured JSON output, rotating logs, proxy support, shell
  completion, a manual page, a hardened systemd user service, and Debian
  packaging.

OneNote files and packages recognized from Graph metadata are not supported.
They are explicitly reported and skipped while other files continue; existing
local copies are preserved and the sync exits with status 2.
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
  Do not add Ubuntu 26.04's `libftxui-dev` 5.0 package to this setup: CMake would
  prefer that older system package over the pinned download.

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
FTXUI and Proxy 4 can be supplied as installed CMake packages. When they are
unavailable, CMake downloads the pinned FTXUI 7.0.3 and ngcpp/proxy 4.1.0
releases. Proxy 4 is header-only. The vcpkg manifest resolves these dependencies
through the `ftxui` and `proxy` ports.

## Build

After cloning the repository, change to its root directory. All build commands
below assume the project root is the current working directory.

### Optional desktop GUI

**Help → About** shows the build version, license, and GitHub project link.

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

Use the **Language** menu to switch between **System language**, **English**, and
**Simplified Chinese** immediately. Existing labels, status messages, settings,
and the window title update without losing unsaved settings drafts or restarting
an active login or synchronization task. The checked menu item shows the selection. Both command-line
and menu selections apply only to the current session and are not saved.
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
their contents in a backup. Saved credentials are explicitly unverified online;
later operations refresh them and report authentication or connectivity errors.
Sync-path validation does not prevent login when authentication/state settings
are usable; synchronization requires valid sync settings and saved credentials.

The main tabs are **Overview / Synchronization / Logs / Settings**. Overview
shows account and directory information plus the current/latest sync result.
Overview has no synchronization buttons; task controls are on the Synchronization tab.

In **Synchronization**, use **Preview changes** to inspect the plan
without changing synchronized files, then **Sync now** to apply it. Both use
saved configuration, not unsaved Settings drafts. A saved `sync.dry_run = true`
keeps **Sync now** in preview mode; the GUI never overrides the remote-deletion
safety limit. Tasks run in a background worker using the same service and state
directory lock as the CLI. The task page shows a concise result/error summary
with details available in the **Logs** tab. The progress bar is blue and displays only a
centered numeric percentage (for example, `50%`). Download progress
includes file/byte counts, speed and an estimated remaining time when available.
Uploads currently have messages but no percentage display.
The percentage measures downloads, not whole-task progress.
Scanning and other phases without measurable totals display an explanatory
message instead of a full-looking indeterminate bar. Overall 100% appears only
after successful task completion.
**Stop** requests cancellation and waits for cleanup. Login, logout and settings
changes are disabled during a task; closing the window cancels and joins it.

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
GUI monitoring and systemd service control remain deferred.

Hover over any setting, its label or browse/override control for a concise tip,
including advanced fields. Tips also provide accessibility descriptions and
switch languages immediately without changing the draft.

Settings phases one and two provide six sub-tabs: **Account and directories**,
**Sync rules**, **Transfers**, **Network**, **Monitor**, and **Advanced and logs**.
They share one draft and global save/import/
reload controls. Save validates with the core TOML parser, preserves comments
and untouched settings, backs up the original, and replaces the file atomically.
Import and reload replace the draft; switching tabs or language preserves it.
Directory changes do not migrate files or credentials. Changes apply to the next
operation; settings are disabled during login or synchronization.

The tabs cover Application/tenant/Drive IDs and directories; sync direction,
deletion/conflict/dotfile policies, selective rules, file-size limit and advanced
`.nosync`/root-file/deletion guard settings; upload/download concurrency,
per-request/aggregate rates, ordering, and advanced chunk/checkpoint/retry/
download validation settings. Sizes use bytes and rates bytes/s (`0` means
unlimited for sizes/rates, but zero remote deletions blocks deletion). Integers
retain TOML's full nonnegative signed 64-bit range.

Phase two adds proxy URL/bypass overrides, username and password/CA file
references, transport timeouts and HTTP/IP modes, Graph throttling, monitor
poll/settle/WebSocket settings, log level/file, file permissions and metadata.
Unchecked proxy overrides inherit the environment independently; an explicitly
empty URL forces direct connections and an empty bypass list overrides `NO_PROXY`.
Passwords are never entered inline; referenced files are checked by runtime
security tools, not opened by the editor. Advanced account settings include
trusted HTTPS auth/Graph endpoints, scopes (changes require signing in again),
and a required mount point. No files, credentials or state are migrated.
Persistent dry-run/force-delete flags and CLI themes are not GUI settings.
GUI monitor workers and service integration remain deferred.

CLI/TUI text, JSON output, configuration keys, and logs are unchanged.
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
onedrive-cpp_<version>-1~ubuntu24.04_amd64.deb
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
sudo apt install -y build-essential devscripts debhelper cmake ninja-build \
  clang-20 libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
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
../onedrive-cpp_0.8.7-1~ubuntu24.04_amd64.deb
```

The Debian changelog carries the native build distribution suffix. When
adding Ubuntu 26.04 support, build from an Ubuntu 26.04 environment with a
the corresponding `-1~ubuntu26.04` changelog version.

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
