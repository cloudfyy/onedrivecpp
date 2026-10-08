# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一款使用 C++26 编写的 OneDrive 同步客户端，支持 Ubuntu
24.04 LTS 及以上版本。项目参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责划分，但未
复制其 D 语言实现。

## 功能

- OAuth 2.0 设备代码认证，并安全隔离各账号的 Token 和资料状态。
- Microsoft Graph Delta 增量同步、多账号和多 Drive 隔离、选择性同步规则及
  显式单文件下载。
- 并发、可续传且带完整性校验的下载，支持原子安装、磁盘空间协调、总速率限制，
  以及阻止或备份本地冲突。
- 并发简单上传和上传会话（upload session），支持稳定快照、持久检查点
  （checkpoint）、崩溃恢复、单文件与总速率限制，以及带条件的 Graph 写入。
- 双向传播文件和目录的创建、修改、删除、移动与重命名；支持通过暂存
  （staging）解决依赖环，并提供可配置的大批量远端删除保护。
- 使用 SQLite 日志（journal）恢复下载、上传、本地与远端移动、远端删除和目录
  创建操作。
- 通过 inotify 长驻监控，支持 Graph 轮询、事件合并、队列溢出恢复和收到信号后
  安全退出。
- 支持试运行（dry-run）、结构化 JSON 输出、日志轮转、代理、Shell 自动补全、
  手册页、加固的 systemd 用户服务和 Debian 打包。

## 文档

[文档中心](docs/README.zh-CN.md)将手册分为三个部分：

1. **开始使用：**[认证](docs/authentication.zh-CN.md)、
   [配置](docs/configuration.zh-CN.md)和[命令使用](docs/usage.zh-CN.md)。
2. **同步与运维：**[同步与恢复](docs/synchronization.zh-CN.md)、选择规则、
   冲突策略、Monitor、诊断和数据库修复。
3. **设计与开发：**[架构](docs/architecture.zh-CN.md)、
   [开发与测试](docs/development.zh-CN.md)以及
   [许可证和项目信息](docs/project.zh-CN.md)。

## 环境要求

### 支持平台

- Ubuntu 24.04 LTS 及更新版本，包括 Ubuntu 26.04 LTS。
- x86_64 或 arm64（取决于 LLVM 和 Ubuntu 构建环境）。
- Clang 20，使用 `-std=c++2c`/CMake `CXX_STANDARD 26`。
- CMake 3.28 或更高版本。
- 使用系统包管理器提供 CLI11、libcurl/OpenSSL、Microsoft GSL、
  nlohmann/json、spdlog/fmt、SQLite 和 toml++ 开发包。

仓库中的 CMake preset 明确使用 `clang++-20` 和 Ninja。
仅安装 `build-essential` 或发行版默认编译器，不能满足这些 preset 的要求。

### 构建环境

在 Ubuntu 24.04 或 26.04 上，从官方仓库安装构建工具：

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
clang-tidy-20 --version
cmake --version
ninja --version
```

#### Ubuntu 26.04 开发要求

- 安装上述软件包前，需要启用官方 `universe` 仓库；Ninja、Clang 20 和部分
  开发库由该仓库提供。安装软件包需要管理员（`sudo`）权限。
- 即使系统已安装其他编译器，也应使用带版本后缀的 Clang 20 编译器、格式化
  和 lint 工具。Ubuntu 26.04 提供的 CMake 4.2 和 Ninja 1.13 满足项目的
  构建工具要求，无需修改 CMake preset。
- 安装 Clang 时仍需保留 `build-essential`，以提供系统 C++ 标准库头文件
  和构建工具。
- 首次配置 CMake 时需要通过 HTTPS 访问 GitHub，供 FetchContent 下载固定
  版本的 FTXUI 7.0.3 和 Proxy 4.1.0。不要在这套环境中额外安装 Ubuntu 26.04
  提供的 `libftxui-dev` 5.0 软件包，否则 CMake 会优先使用较旧的系统版本，
  而非下载固定版本。

安装完成后，使用下文的 Debug 或 Release 构建与 CTest 命令验证。
仅完成软件包安装，并不代表项目已在本机通过编译和测试。

CMake 会使用所选编译器检查系统 fmt/spdlog。当 Clang 遇到 fmt 9/10 已知的
编译期格式字符串问题时，会将一个小范围的
[上游修复](https://github.com/fmtlib/fmt/commit/6797f0c) 回移到构建目录中的
已安装头文件副本。系统头文件不会被修改，程序仍动态链接系统 fmt/spdlog 库。
配置阶段会验证合法格式可以编译、非法格式仍被拒绝；补丁无法安全应用时会报错
停止。系统头文件通过检查后，不再使用该兼容副本。

提交前必须格式化本次改动涉及的 C/C++ 代码行。先暂存源文件和头文件，再从仓库
根目录运行指定版本的 Git 集成。该命令会读取仓库中的 `.clang-format`，且不会
重排无关的历史代码：

```bash
git add path/to/modified.cpp path/to/modified.hpp
git-clang-format-20 --binary clang-format-20 --staged
git add path/to/modified.cpp path/to/modified.hpp
```

新文件可以直接使用 `clang-format-20 -i` 完整格式化。不要使用无版本后缀的
命令或其他主版本，因为输出可能与 Clang 20 不一致。

构建系统从操作系统解析依赖库，并采用动态链接。因此，Debian 可以通过安全更新
替换 libcurl、OpenSSL、SQLite、spdlog 和 fmt，无需重新构建 `onedrive-cpp`。
FTXUI 和 Proxy 4 可以由已安装的 CMake 包提供；如果找不到，CMake 会分别下载
固定版本的 FTXUI 7.0.3 和 ngcpp/proxy 4.1.0。Proxy 4 是纯头文件库
（header-only）。vcpkg manifest 通过 `ftxui` 和 `proxy` port 解析这些依赖。

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
./build/release/onedrive-cpp transfer sync --dry-run
./build/release/onedrive-cpp --help
```

### 完整 Release 重编译

只需删除 Release 构建目录，即可让 CMake 和 Ninja 从空白状态重新配置并编译：

```bash
rm -rf build/release
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/onedrive-cpp transfer sync --dry-run
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

使用默认前缀时，可执行文件、示例配置、systemd 用户服务、中英文手册页和
Shell 自动补全都会安装到 `/usr/local`。如需更改前缀，请在配置阶段设置
`CMAKE_INSTALL_PREFIX`。

### 快速生成 CPack 包

使用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。CMake 会从 `/etc/os-release` 读取 `ID` 和
`VERSION_ID`，因此 Ubuntu 24.04 amd64 构建命名为：

```text
onedrive-cpp_<version>-1~ubuntu24.04_amd64.deb
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
sudo apt install -y build-essential devscripts debhelper cmake ninja-build \
  clang-20 libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
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
../onedrive-cpp_0.8.7-1~ubuntu24.04_amd64.deb
```

Debian changelog 保留原生构建的发行版后缀。将来增加 Ubuntu 26.04 支持时，应
在 Ubuntu 26.04 环境中使用对应的 `-1~ubuntu26.04` changelog 版本构建。

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
