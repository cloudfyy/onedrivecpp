# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一个面向 Ubuntu 24.04 LTS 及以上版本的 C++26 OneDrive
同步客户端。它参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责拆分方式，
但不复制其 D 语言实现。

## 功能

- OAuth 2.0 设备代码认证，以及安全隔离的账号 Token 和资料状态。
- Microsoft Graph Delta 增量同步、多账号和多 Drive 隔离、选择性同步规则及
  显式单文件下载。
- 并发、可续传且带完整性校验的下载，支持原子安装、磁盘空间协调、总速率限制，
  以及阻止或备份本地冲突。
- 并发简单上传和 upload session，支持稳定快照、持久 checkpoint、崩溃恢复、
  单文件及总速率限制和条件 Graph 写入。
- 文件与目录的双向创建、修改、删除、移动和重命名传播，包括依赖环 staging 和
  可配置的大批量远端删除保护。
- 基于 SQLite journal 的下载、上传、本地与远端移动、远端删除和目录创建恢复。
- inotify 长驻监控、Graph 轮询、事件合并、队列溢出恢复和干净信号退出。
- dry-run、结构化 JSON 输出、轮转日志、代理、Shell 自动补全、手册页、加固的
  systemd 用户服务和 Debian 打包。

详细文档：

- [架构](docs/architecture.zh-CN.md)
- [使用与命令输出](docs/usage.zh-CN.md)
- [配置](docs/configuration.zh-CN.md)
- [Microsoft 认证](docs/authentication.zh-CN.md)
- [同步与恢复](docs/synchronization.zh-CN.md)
- [开发与测试](docs/development.zh-CN.md)
- [许可证、法律与品牌](docs/project.zh-CN.md)

## 环境要求

### 支持平台

- Ubuntu 24.04 LTS 及更新版本。
- x86_64 或 arm64（取决于 LLVM 和 Ubuntu 构建环境）。
- Clang 20，使用 `-std=c++2c`/CMake `CXX_STANDARD 26`。
- CMake 3.28 或更高版本。
- 使用系统包管理器提供 CLI11、libcurl/OpenSSL、nlohmann/json、
  spdlog/fmt、SQLite 和 toml++ 开发包。

Ubuntu 24.04 的官方仓库已经提供项目所需的 CMake、Ninja 和 Clang 20。
项目不使用默认的 GCC 13，因为它的 C++26 支持不足以满足当前配置。

### 构建环境

从 Ubuntu 24.04 官方仓库安装构建工具：

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 clang-format-20 clang-tidy-20 \
  zip unzip tar pkg-config \
  libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

确认版本：

```bash
clang++-20 --version
clang-format-20 --version
cmake --version
```

提交前必须格式化本次修改的 C/C++ 行。先暂存源文件和头文件，再从仓库根目录
运行固定版本的 Git 集成，使其读取仓库内的 `.clang-format`，同时避免重排无关
的历史代码：

```bash
git add path/to/modified.cpp path/to/modified.hpp
git-clang-format-20 --binary clang-format-20 --staged
git add path/to/modified.cpp path/to/modified.hpp
```

新文件可以直接使用 `clang-format-20 -i` 完整格式化。不要使用无版本后缀的
命令或其他主版本，因为输出可能与 Clang 20 不一致。

编译库从操作系统解析并动态链接。这样 Debian 的安全更新可以替换 libcurl、
OpenSSL、SQLite、spdlog 和 fmt，而无需重新构建 `onedrive-cpp`。
Proxy 4 是 header-only 库。CMake 优先使用已安装的 `msft_proxy4` 包，否则下载
固定版本的 ngcpp/proxy 4.1.0；vcpkg manifest 则通过 `proxy` port 解析它。

## 编译

克隆仓库后请进入仓库根目录。以下所有构建命令均假定当前工作目录为项目根目录。

### Release 构建

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

生成的程序位于 `build/release/onedrive-cpp`。使用以下命令验证：

```bash
./build/release/onedrive-cpp --version
./build/release/onedrive-cpp sync --dry-run
./build/release/onedrive-cpp --help
```

### 完整 Release 重编译

仅删除 Release 构建目录，使 CMake 和 Ninja 从空白状态重新配置和编译：

```bash
rm -rf build/release
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/onedrive-cpp sync --dry-run
```

Ninja 默认自动并行编译。如需明确指定并行任务数：

```bash
cmake --build --preset release --parallel 4
```

### Debug 构建

Debug 与 Release 使用相互独立的构建目录：

```bash
rm -rf build/debug
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Debug 程序位于 `build/debug/onedrive-cpp`。

## 安装

### 从 CMake 构建安装

把 Release 构建安装到配置的前缀：

```bash
sudo cmake --install build/release
```

默认前缀会把可执行文件、示例配置、systemd 用户服务、中英文手册页和 Shell
自动补全安装到 `/usr/local`。需要其他前缀时，在配置阶段设置
`CMAKE_INSTALL_PREFIX`。

### 快速生成 CPack 包

使用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。CMake 会从 `/etc/os-release` 读取 `ID` 和
`VERSION_ID`，因此 Ubuntu 24.04 amd64 构建命名为：

```text
onedrive-cpp_0.8.5-1~ubuntu24.04_amd64.deb
```

在跨发行版构建环境中，可在配置时显式覆盖检测结果：

```bash
cmake --preset release \
  -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04
```

Ubuntu 26.04 包仍必须在 Ubuntu 26.04 环境中构建和测试；仅修改后缀不会改变
二进制 ABI。

### 构建正式 Debian 包

安装打包依赖：

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

在项目根目录执行：

```bash
source "$HOME/.profile"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` 会执行 CMake 配置、编译和测试。生成的 `.deb` 位于项目
目录的上一级，例如：

```text
../onedrive-cpp_0.8.5-1~ubuntu24.04_amd64.deb
```

Debian changelog 保存原生构建发行版后缀。将来增加 Ubuntu 26.04 支持时，应在
Ubuntu 26.04 环境中使用 `0.8.5-1~ubuntu26.04` changelog 版本构建。

安装并检查：

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
man onedrive-cpp
```

如需检查 Debian 策略问题：

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```
