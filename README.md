# onedrive-cpp

English | [简体中文](README.zh-CN.md)

`onedrive-cpp` is a C++26 OneDrive synchronization client scaffold for
Ubuntu 22.04 and later. Its separation of responsibilities is inspired by
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

- Ubuntu 22.04 LTS, 24.04 LTS, and later.
- x86_64 or arm64, depending on the LLVM and Ubuntu build environment.
- Clang 20 with `-std=c++2c` / CMake `CXX_STANDARD 26`.
- CMake 3.25 or later.

The GCC and CMake versions included with Ubuntu 22.04 are not sufficient for
this project's C++26 configuration. The following instructions therefore use
the official LLVM and Kitware APT repositories.

## Set Up the Build Environment

Install the base tools:

```bash
sudo apt update
sudo apt install -y ca-certificates curl gnupg lsb-release software-properties-common wget
```

Install Clang 20:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
rm llvm.sh
```

Install a recent CMake version on Ubuntu 22.04 or 24.04:

```bash
curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
  | gpg --dearmor \
  | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null

echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/kitware.list

sudo apt update
sudo apt install -y cmake ninja-build clang-20
```

Verify the installed versions:

```bash
clang++-20 --version
cmake --version
```

## Build and Test Locally

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a safe configuration dry run:

```bash
./build/onedrive-cpp sync --dry-run
./build/onedrive-cpp --help
```

You can also use CPack to quickly create a package that has not undergone full
Debian policy checks:

```bash
cd build
cpack -G DEB
```

The generated package is placed in `build/`.

## Build an Official DEB Package

Install the packaging dependencies:

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

Run the following commands from the project root:

```bash
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

## Copy the Source to a Remote Build Server

After installing OpenSSH Client on Windows 10 or 11, run the deployment script
from the project root in PowerShell:

```powershell
.\scripts\deploy.ps1
```

The script copies the project source to:

```text
yingying@mydoor.eastasia.cloudapp.azure.com:/home/yingying/onedrivecpp
```

It excludes `build/`, `.git/`, `.cache/`, and `compile_commands.json`. Files
with the same names on the remote server are overwritten, but remote files
that do not exist locally are not deleted. On the first connection, you must
confirm the server fingerprint. Authentication uses SSH Agent, a default SSH
key, or an interactive password; the script does not store credentials.

To specify a private key:

```powershell
.\scripts\deploy.ps1 -IdentityFile "$HOME\.ssh\id_ed25519"
```

After the upload completes, connect to the server and build as described
above:

```powershell
ssh yingying@mydoor.eastasia.cloudapp.azure.com
```

```bash
cd /home/yingying/onedrivecpp
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
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
