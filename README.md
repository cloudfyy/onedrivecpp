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

- Ubuntu 24.04 LTS and later.
- x86_64 or arm64, depending on the LLVM and Ubuntu build environment.
- Clang 20 with `-std=c++2c` / CMake `CXX_STANDARD 26`.
- CMake 3.28 or later.
- Development packages for CLI11, libcurl/OpenSSL, nlohmann/json, spdlog/fmt,
  SQLite, and toml++ from the system package manager.

Ubuntu 24.04 provides the required CMake, Ninja, and Clang 20 packages through
its official repositories. GCC 13 is not used because its C++26 support is not
sufficient for this project configuration.

### Build environment

Install the build tools from the Ubuntu 24.04 repositories:

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
cmake --version
```

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
Proxy 4 is header-only. CMake uses an installed `msft_proxy4` package when
available and otherwise downloads the pinned ngcpp/proxy 4.1.0 release.
The vcpkg manifest resolves it through the `proxy` port.

## Build

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
