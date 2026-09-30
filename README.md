# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client scaffold for
Ubuntu 24.04 LTS and later. Its separation of responsibilities is inspired by
[abraunegg/onedrive](https://github.com/abraunegg/onedrive), but it does not
copy the reference project's D implementation.

> The current version provides a buildable architecture scaffold,
> configuration loading, a CLI, dry-run support, a local state abstraction, a
> systemd user service, and Debian packaging. Microsoft Graph OAuth, network
> transport, SQLite persistence, and actual file synchronization have not yet
> been implemented. Non-dry-run synchronization exits with an explicit error.

## Architecture

```text
CLI / Application
       |
       +-- Config
       +-- Monitor (file system event entry point)
       +-- SyncEngine (synchronization orchestration)
               |
               +-- GraphClient (Microsoft Graph boundary)
               +-- ItemDatabase (local state boundary)
```

The directories correspond to the responsibilities of
`main/config/curlEngine/onedrive/sync/itemdb/monitor` in the reference project:

- `src/app`: CLI parsing and application lifecycle.
- `src/config`: Configuration file loading and validation.
- `src/graph`: Microsoft Graph API boundary.
- `src/storage`: Remote ID, ETag, and local path state.
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

Add the `VCPKG_ROOT` export to your shell profile to make it persistent.
The committed `vcpkg.json` manifest pins the registry baseline and is the
single source of truth for C++ library dependencies. Its dependency list is
currently empty because the scaffold does not yet link third-party libraries.
vcpkg automatically installs declared dependencies while CMake configures the
project.

## Build and Test Locally

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Run a safe configuration dry run:

```bash
./build/release/onedrive-cpp sync --dry-run
./build/release/onedrive-cpp --help
```

You can also use CPack to quickly create a package that has not undergone full
Debian policy checks:

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

The generated package is placed in the current directory.

## Build an Official DEB Package

Install the packaging dependencies:

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

Run the following commands from the project root:

```bash
export VCPKG_ROOT="$HOME/vcpkg"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` configures the project with CMake, builds it, and runs the
tests. The generated `.deb` is placed in the parent directory of the project,
for example:

```text
../onedrive-cpp_0.1.0_amd64.deb
```

Install and verify the package:

```bash
sudo apt install ../onedrive-cpp_0.1.0_$(dpkg --print-architecture).deb
onedrive-cpp --version
```

To check for Debian policy issues:

```bash
sudo apt install -y lintian
lintian ../onedrive-cpp_0.1.0_*.changes
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

After installing the DEB package, enable the user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

The service currently runs the monitor scaffold. It does not access OneDrive
or modify the synchronization directory.

## Suggested Next Steps

1. Implement Graph HTTP transport and device-code OAuth with libcurl.
2. Replace the in-memory `ItemDatabase` with SQLite.
3. Implement the delta API, conflict policies, and safe atomic file
   replacement.
4. Connect the monitor to inotify and add integration tests for the Graph and
   file system boundaries.

## License

GPL-3.0-or-later. The reference project also uses GPLv3, which makes this
license suitable for future reuse or adaptation of its design in compliance
with the license terms.
