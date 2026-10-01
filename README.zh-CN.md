# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一个面向 Ubuntu 24.04 LTS 及以上版本的 C++26 OneDrive
同步客户端项目骨架。它参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责拆分方式，
但不复制其 D 语言实现。

> 当前版本提供可编译的架构骨架、配置加载、CLI、Microsoft 设备代码认证、
> HTTP 传输层、SQLite 状态持久化、dry-run、systemd 用户服务和 Debian 打包。
> Microsoft Graph 文件操作和真实文件同步尚未实现；非 dry-run 同步会明确
> 报错。

## 架构

```text
CLI / Application
       |
       +-- Config
       +-- DeviceAuth / TokenStore
       +-- HttpTransport (libcurl)
       +-- Monitor (文件系统事件入口)
       +-- SyncEngine (同步编排)
               |
               +-- GraphClient (Microsoft Graph 边界)
               +-- ItemDatabase (本地状态边界)
```

目录与参考项目中的 `main/config/curlEngine/onedrive/sync/itemdb/monitor`
职责相对应：

- `src/app`：CLI 解析和应用生命周期。
- `src/auth`：设备代码 OAuth、Token 刷新和安全持久化。
- `src/config`：配置文件加载和校验。
- `src/graph`：Microsoft Graph API 访问边界。
- `src/http`：强类型 libcurl HTTP 传输层。
- `src/storage`：使用 SQLite 持久化远端 ID、ETag 与本地路径状态。
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

持久化 `VCPKG_ROOT`，并在当前 Shell 中加载：

```bash
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> "$HOME/.profile"
source "$HOME/.profile"
```

仓库中的 `vcpkg.json` manifest 固定了 registry baseline，是 C++ 库依赖的
唯一事实来源。目前它提供 CLI11、libcurl、OpenSSL、nlohmann/json、spdlog 和
SQLite。CMake 配置项目时，vcpkg 会自动安装 manifest 中声明的依赖。

## 本地构建与测试

所有构建命令均在项目根目录执行：

```bash
cd /home/yingying/onedrivecpp
source "$HOME/.profile"
```

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

### 日志

运行诊断默认以 `info` 级别写入标准错误。systemd 会自动收集该输出：

```bash
journalctl --user -u onedrive-cpp.service -f
```

每个子命令都支持 `--log-level` 和 `--log-file`：

```bash
onedrive-cpp sync --dry-run --log-level debug
onedrive-cpp monitor --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
```

支持 `trace`、`debug`、`info`、`warn`、`error`、`critical` 和 `off`。
日志文件达到 5 MiB 时轮转，并保留三个旧文件。不得把认证 token、设备代码或
Authorization header 写入日志。

### 完整 Release 重编译

仅删除 Release 构建目录，使 CMake、vcpkg 和 Ninja 从空白状态重新配置和
编译：

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

每个构建目录都有独立的 `vcpkg_installed` 目录。因此，删除
`build/release` 或 `build/debug` 后，vcpkg 会在下次 CMake 配置时重新安装
对应配置的依赖。

## 构建 DEB 包

### 快速生成 CPack 包

使用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。

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
../onedrive-cpp_<版本>_<架构>.deb
```

安装并检查：

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
```

如需检查 Debian 策略问题：

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```

## 配置与 systemd

默认优先读取 `~/.config/onedrive-cpp/config`；文件不存在时使用内置默认值。
系统示例位于 `/etc/onedrive-cpp/onedrive-cpp.conf`。首次使用可执行：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.conf ~/.config/onedrive-cpp/config
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config
```

远端 ID、ETag 和本地路径状态保存在
`<state_directory>/items.sqlite3`。数据库使用 SQLite WAL 模式，并在程序
启动时加载。

### Microsoft 认证

客户端使用 OAuth 2.0 设备授权流程。必须在 Microsoft Entra 中注册自己的公共
客户端应用；不要创建客户端密钥。

#### 注册应用

Microsoft 当前要求账号具有有效的 Azure 订阅、可访问的 Microsoft Entra 租户，
并拥有注册应用的权限。如果只有 Outlook.com/Hotmail 个人账号且无法打开
**App registrations**，请先创建
[免费 Azure 账号](https://azure.microsoft.com/zh-cn/pricing/purchase-options/azure-account)，
使用其 **Default Directory**，或者请租户管理员分配 Application Developer
角色。

1. 登录 [Microsoft Entra 管理中心](https://entra.microsoft.com/)。
2. 打开 **Entra ID > 应用注册（App registrations）> 新注册
   （New registration）**。
3. 输入应用名称，例如 `onedrive-cpp`。
4. 选择支持的账号类型：
   - 如果同时支持工作/学校账号和个人 Microsoft 账号，选择
     **Any Entra ID Tenant + Personal Microsoft accounts**，并在配置中使用
     `azure_tenant_id=common`。
   - 如果只支持个人 Microsoft 账号，选择 **Personal accounts only**，并使用
     `azure_tenant_id=consumers`。
   - 如果只供一个组织使用，选择 **Single tenant**，并使用该目录的 Tenant ID。
5. 点击 **注册（Register）**。
6. 在应用的 **概述（Overview）** 页面复制 **Application (client) ID**。
   `application_id` 不应填写 Object ID 或 Directory ID。
7. 打开 **身份验证（Authentication）> 高级设置（Advanced settings）**，
   将 **Allow public client flows** 设置为 **Yes** 并保存。

设备代码流不需要 Redirect URI。公共客户端中也不要添加 Client Secret，因为
桌面或命令行程序无法安全保存嵌入的客户端密钥。

对应的 Microsoft 官方文档：

- [在 Microsoft Entra ID 中注册应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/quickstart-register-app)
- [配置桌面和公共客户端应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 设备授权流程](https://learn.microsoft.com/zh-cn/entra/identity-platform/v2-oauth2-device-code)

#### 配置权限

打开 **API 权限（API permissions）> 添加权限（Add a permission）>
Microsoft Graph > 委托的权限（Delegated permissions）**。

个人 OneDrive 账号建议从最小权限开始：

```text
Files.ReadWrite
```

客户端还会请求 `offline_access`，以便 Microsoft 返回 refresh token。只有在
组织版 OneDrive、共享文档库或 SharePoint 场景确实需要时，才添加更广泛的
委托权限：

```text
Files.ReadWrite.All
Sites.ReadWrite.All
```

组织租户的策略可能要求管理员批准权限。个人 Microsoft 账号通常在设备登录时
由用户自行同意。最新权限定义请参阅
[Microsoft Graph 权限参考](https://learn.microsoft.com/zh-cn/graph/permissions-reference)。

#### 配置 onedrive-cpp

如果用户配置文件尚不存在，先创建：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.conf ~/.config/onedrive-cpp/config
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config
```

注册类型为 **仅个人 Microsoft 账号（Personal Microsoft accounts only）**
的应用使用：

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=consumers
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite offline_access
```

如果应用注册类型为 **任何 Entra ID 租户和个人 Microsoft 账号
（Any Entra ID Tenant + Personal Microsoft accounts）**，即使本次登录使用
个人账号，也应使用 `common`：

```ini
application_id=YOUR_APPLICATION_CLIENT_ID
azure_tenant_id=common
auth_endpoint=https://login.microsoftonline.com
auth_scope=Files.ReadWrite offline_access
```

单租户组织应用应将 `common` 替换为 Directory (tenant) ID。只有组织场景确实
需要时才添加 `Files.ReadWrite.All` 或 `Sites.ReadWrite.All`；
`Sites.ReadWrite.All` 不支持个人 Microsoft 账号。

#### 授权客户端

使用以下命令启动设备授权：

```bash
onedrive-cpp auth
```

打开终端显示的网址，输入用户代码，使用与应用支持账号类型相符的账号登录，
并确认所请求的权限。成功后，refresh token 会以原子方式保存到
`<state_directory>/refresh_token`，权限限制为仅文件所有者可读写的 `0600`。

如果 Microsoft 提示账号类型不受支持，请检查应用注册中的
**Supported account types**：仅个人账号注册应使用 `consumers`，同时支持个人
和组织账号的注册应使用 `common`。

如果程序显示 `https://www.microsoft.com/link`，但该页面立即提示刚生成的代码
无效或已过期，请检查是否把同时支持个人和组织账号的应用错误配置成了
`azure_tenant_id=consumers`。应改为：

```ini
azure_tenant_id=common
```

重新运行 `onedrive-cpp auth`，并且只在新显示的
`https://login.microsoft.com/device` 页面中使用本次新代码。之前生成的设备代码
不能重复使用。只有应用的 Supported account type 确实是
**Personal Microsoft accounts only** 时才使用 `consumers`。

如果设备代码已被接受并进入账号登录，但之后 Microsoft 又提示代码已过期，
同时终端仍停留在 `Waiting for authorization...`，应删除当前账号类型不支持的
权限。个人 Microsoft 账号应使用：

```ini
auth_scope=Files.ReadWrite offline_access
```

修改权限后必须重新运行 `onedrive-cpp auth`；已有设备代码仍绑定原来的权限，
无法修复或重复使用。

使用以下命令删除已保存的认证：

```bash
onedrive-cpp logout
```

OAuth 客户端已经实现 refresh token 轮换，但尚未将取得的访问令牌接入 Graph
文件操作。

安装 DEB 后启用用户服务：

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

当前服务运行 monitor 骨架，不会访问 OneDrive 或修改同步目录。

## 后续实现建议

1. 使用 OAuth access token 发起经过认证的 Microsoft Graph Drive 和 Item
   请求。
2. 实现 delta API、冲突策略和安全的原子文件替换。
3. 使用 inotify 接入 monitor，并为 Graph 和文件系统边界增加集成测试。

## 许可证

GPL-3.0-or-later。参考项目同样采用 GPLv3，因此该许可证也便于未来在满足
许可证要求的前提下复用或改写其设计。

## 法律与品牌材料

- [服务条款](TERMS.md)
- [隐私声明](PRIVACY.md)
- 项目 Logo：[SVG](assets/onedrive-cpp-logo.svg) |
  [512x512 PNG](assets/onedrive-cpp-logo.png)

法律文件是项目维护者草案，不构成法律建议。正式公开或商业发布前，应根据
实际运营主体和适用司法管辖区进行审核。onedrive-cpp 是独立项目，与 Microsoft
不存在隶属或背书关系。
