# onedrive-cpp

`onedrive-cpp` 是一个面向 Ubuntu 22.04 及以上版本的 C++26 OneDrive
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

- Ubuntu 22.04 LTS、24.04 LTS 及更新版本。
- x86_64 或 arm64（取决于 LLVM 和 Ubuntu 构建环境）。
- Clang 20，使用 `-std=c++2c`/CMake `CXX_STANDARD 26`。
- CMake 3.25 或更高版本。

Ubuntu 22.04 自带的 GCC 和 CMake 版本不足以完成此项目的 C++26 配置，
因此下面使用 LLVM 官方仓库和 Kitware 官方 APT 仓库。

## 配置构建环境

安装基础工具：

```bash
sudo apt update
sudo apt install -y ca-certificates curl gnupg lsb-release software-properties-common wget
```

安装 Clang 20：

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
rm llvm.sh
```

安装新版 CMake（Ubuntu 22.04/24.04 均适用）：

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

确认版本：

```bash
clang++-20 --version
cmake --version
```

## 本地构建与测试

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

运行安全的配置演练：

```bash
./build/onedrive-cpp sync --dry-run
./build/onedrive-cpp --help
```

也可以用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cd build
cpack -G DEB
```

生成的文件位于 `build/`。

## 构建正式 DEB 包

安装打包依赖：

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

在项目根目录执行：

```bash
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

## 复制源码到远程构建服务器

Windows 10/11 安装 OpenSSH Client 后，可以在项目根目录通过 PowerShell
运行部署脚本：

```powershell
.\scripts\deploy.ps1
```

脚本会将项目源码复制到：

```text
yingying@mydoor.eastasia.cloudapp.azure.com:/home/yingying/onedrivecpp
```

它会排除 `build/`、`.git/`、`.cache/` 和 `compile_commands.json`，并覆盖远端
的同名文件，但不会删除远端存在而本地不存在的文件。首次连接时需确认服务器
指纹；身份验证使用 SSH Agent、默认 SSH 密钥或交互式密码，不会在脚本中保存
凭据。

如需指定私钥：

```powershell
.\scripts\deploy.ps1 -IdentityFile "$HOME\.ssh\id_ed25519"
```

上传完成后，可登录服务器并按前述步骤构建：

```powershell
ssh yingying@mydoor.eastasia.cloudapp.azure.com
```

```bash
cd /home/yingying/onedrivecpp
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
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
