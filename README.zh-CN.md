# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一个面向 Ubuntu 24.04 LTS 及以上版本的 C++26 OneDrive
同步客户端项目骨架。它参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责拆分方式，
但不复制其 D 语言实现。

> 当前版本提供可编译的架构骨架、配置加载、CLI、dry-run、本地状态抽象、
> systemd 用户服务和 Debian 打包。Microsoft Graph OAuth、网络传输、SQLite
> 持久化和真实文件同步尚未实现；非 dry-run 同步会明确报错。

## 架构

```text
CLI / Application
       |
       +-- Config
       +-- Monitor (文件系统事件入口)
       +-- SyncEngine (同步编排)
               |
               +-- GraphClient (Microsoft Graph 边界)
               +-- ItemDatabase (本地状态边界)
```

目录与参考项目中的 `main/config/curlEngine/onedrive/sync/itemdb/monitor`
职责相对应：

- `src/app`：CLI 解析和应用生命周期。
- `src/config`：配置文件加载和校验。
- `src/graph`：Microsoft Graph API 访问边界。
- `src/storage`：远端 ID、ETag 与本地路径状态。
- `src/sync`：差异计算和同步流程编排入口。
- `src/monitor`：长驻监控模式入口。
- `packaging/systemd`：systemd 用户服务。
- `debian`：Ubuntu/Debian 原生包元数据。

## 支持范围

- Ubuntu 24.04 LTS 及更新版本。
- x86_64 或 arm64（取决于 LLVM 和 Ubuntu 构建环境）。
- Clang 20，使用 `-std=c++2c`/CMake `CXX_STANDARD 26`。
- CMake 3.28 或更高版本。
- 使用 vcpkg manifest 模式管理 C++ 依赖。

Ubuntu 24.04 的官方仓库已经提供项目所需的 CMake、Ninja 和 Clang 20。
项目不使用默认的 GCC 13，因为它的 C++26 支持不足以满足当前配置。

## 配置构建环境

从 Ubuntu 24.04 官方仓库安装构建工具：

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 zip unzip tar pkg-config
```

确认版本：

```bash
clang++-20 --version
cmake --version
```

安装并引导 vcpkg：

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
```

建议将 `VCPKG_ROOT` 的导出命令加入 shell 配置文件，使其永久生效。
仓库中的 `vcpkg.json` manifest 固定了 registry baseline，是 C++ 库依赖的
唯一事实来源。当前依赖列表为空，因为项目骨架尚未链接第三方库。CMake
配置项目时，vcpkg 会自动安装 manifest 中声明的依赖。

## 本地构建与测试

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

运行安全的配置演练：

```bash
./build/release/onedrive-cpp sync --dry-run
./build/release/onedrive-cpp --help
```

也可以用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。

## 构建正式 DEB 包

安装打包依赖：

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

在项目根目录执行：

```bash
export VCPKG_ROOT="$HOME/vcpkg"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` 会执行 CMake 配置、编译和测试。生成的 `.deb` 位于项目
目录的上一级，例如：

```text
../onedrive-cpp_0.1.0_amd64.deb
```

安装并检查：

```bash
sudo apt install ../onedrive-cpp_0.1.0_$(dpkg --print-architecture).deb
onedrive-cpp --version
```

如需检查 Debian 策略问题：

```bash
sudo apt install -y lintian
lintian ../onedrive-cpp_0.1.0_*.changes
```

## 配置与 systemd

默认优先读取 `~/.config/onedrive-cpp/config`；文件不存在时使用内置默认值。
系统示例位于 `/etc/onedrive-cpp/onedrive-cpp.conf`。首次使用可执行：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.conf ~/.config/onedrive-cpp/config
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config
```

安装 DEB 后启用用户服务：

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

当前服务运行 monitor 骨架，不会访问 OneDrive 或修改同步目录。

## 后续实现建议

1. 使用 libcurl 实现 Graph HTTP 传输和设备代码 OAuth。
2. 使用 SQLite 替换内存 `ItemDatabase`。
3. 实现 delta API、冲突策略和安全的原子文件替换。
4. 使用 inotify 接入 monitor，并为 Graph 和文件系统边界增加集成测试。

## 许可证

GPL-3.0-or-later。参考项目同样采用 GPLv3，因此该许可证也便于未来在满足
许可证要求的前提下复用或改写其设计。
