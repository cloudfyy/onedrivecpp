# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一款使用 C++26 编写的 OneDrive 同步客户端，支持 Ubuntu
24.04 LTS 及以上版本。项目参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责划分，但未
复制其 D 语言实现。

当前版本为 **1.0.2**。CLI、TUI、GUI、User-Agent 和 CPack 版本统一从
`vcpkg.json` 的 `version-semver` 生成；Debian 源码打包的发行版版本另记于
`debian/changelog`。

## 功能

- [OAuth 2.0 认证](docs/authentication.zh-CN.md)：CLI/TUI 使用设备代码登录，
  GUI 使用系统浏览器、授权码 + PKCE（S256）及本地回环回调登录；按账号共享
  凭据、自动刷新 access token，refresh token 仅允许文件所有者读写。
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
- 通过 inotify 长驻监控，支持 Graph WebSocket 通知、轮询兜底、事件合并、
  队列溢出恢复和收到信号后安全退出。
- 可选中英文 Qt GUI，支持即时切换语言、带校验的设置编辑与导入，复用 Monitor
  所有权与控制，提供持久阻止项查询及监控会话／本轮同步统计。
- CLI 与 GUI 共用已完成文件操作历史，显示时间、动作、移动前后路径，支持配置
  保留上限并明确显示省略数量。
- 提供 Console 与交互式 FTXUI 终端界面，支持试运行（dry-run）、结构化 JSON
  输出、日志轮转、代理、Shell 自动补全、手册页、加固的 systemd 用户服务和
  Debian 打包。

不支持通过 Graph 元数据识别出的 OneNote 文件及包。程序会明确报告并跳过这些项目，
继续同步其他文件，保留已有本地副本；同步退出码为 3。
详见[不支持的格式](docs/synchronization.zh-CN.md#不支持的格式)。

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
  版本的 FTXUI 7.0.3 和 Proxy 4.1.0。Ubuntu 26.04 提供的 `libftxui-dev`
  5.0 版本过旧，CMake 会拒绝该版本，改用固定源码。

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
开发构建可使用已安装的 FTXUI 7.x（至少 7.0.3）和 Proxy 4.x（至少 4.1）
CMake 包；否则使用 `packaging/dependencies.json` 中记录的固定源码归档，
必要时联网下载。Proxy 4 是纯头文件库（header-only）。
正式发布构建使用校验后的随包归档，不选择这两个依赖的系统副本；
随包源码的原始许可证安装到 `share/doc/onedrive-cpp/licenses`。
vcpkg manifest 通过 `ftxui` 和 `proxy` port 解析这些依赖。

## 编译

克隆仓库后请进入仓库根目录。以下所有构建命令均假定当前工作目录为项目根目录。

### 可选桌面 GUI

页签栏右侧的 **关于** 按钮显示当前构建版本、许可证和 GitHub 项目链接，
不再单独显示菜单栏。

GUI 要求 Qt 6.8 或更高版本，以及 Linguist 工具和 Qt 简体中文翻译包。
在 Ubuntu 26.04 上：

```bash
sudo apt install -y qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-translations-l10n
cmake --preset release -DONEDRIVE_BUILD_GUI=ON
cmake --build --preset release --target onedrive-cpp-gui
./build/release/onedrive-cpp-gui
# 可选的启动语言参数（不会弹出语言选择对话框）：
./build/release/onedrive-cpp-gui --language en
./build/release/onedrive-cpp-gui --language=zh_CN
./build/release/onedrive-cpp-gui --language auto
```

GUI 默认（`--language auto`）启动时跟随系统语言：简体中文环境显示 **OneDrive 客户端**，英语或其他
不支持的语言环境显示 **onedrive client**。概览、设置、登录状态、对话框和
浏览器回调消息均有翻译。应用与 Qt 翻译内嵌在可执行文件中，运行时不需要额外
翻译文件。`--language` 仅接受 `auto`、`en` 和 `zh_CN`；不支持的值或未知参数
会明确报错并退出。`--help` 和 `--version` 输出信息后退出，不会打开窗口。

运行时可通过页签栏右侧的 **语言** 下拉框选择 **系统语言**、**英语** 或 **简体中文**，
立即更新现有标签、状态消息、设置界面和窗口标题，不丢失未保存的设置草稿，
也不重启正在进行的登录或同步。下拉框显示当前选择。命令行和下拉框选择仅在本次
运行有效，不会保存。登录、同步和监控期间，即使设置页被禁用，语言选择和 **关于**
仍可使用。更改系统语言后请重启应用；原生对话框和外部浏览器可能使用各自的
系统语言设置。

配置、文件系统、后端及服务器的原始错误详情会保留，可能仍为英语；GUI 提供的
错误上下文会翻译。CLI/TUI 文本、JSON 输出、配置键和日志保持不变。
启动仅检查本地状态，不会在线验证凭据、打开浏览器或弹出登录对话框。
配置文件不存在时使用默认值并提示完成设置（登录前在 **设置 → 账号与目录**
填写并保存 **应用 ID**）。配置无效或账号状态无法读取时，设置仍可使用，
登录暂时禁用。可导入有效 TOML，或修复当前文件后点击 **重新加载配置**。
损坏的配置不会被静默覆盖，导入时会备份原内容。启动时仅报告是否在本地找到
保存的登录信息；联网操作时检查登录状态、按需刷新凭据，并报告身份验证或连接错误。
身份验证及状态配置可用时，同步路径验证不会阻止登录；同步要求有效的同步设置和
已保存凭据。

主界面分为 **概览 / 同步 / 日志 / 设置 / 文件**。概览显示账号、关键目录和当前／最近一次
同步结果，不提供同步按钮；任务控制集中在同步页。

在 **同步状态** 页中，使用 **预览变更** 查看计划而不修改同步文件，再用
**立即同步** 应用变更。两者读取已保存的配置，不使用设置页中未保存的草稿。
保存的 `sync.dry_run = true` 会让“立即同步”仍以预览模式运行；GUI 不会绕过
远端删除数量保护。任务在后台线程运行，复用 CLI 的应用服务和状态目录运行锁。
任务页显示简短结果／错误摘要，详情可在**日志**页查看；页面显示同步方向和单次运行方式，
分别展示上传、下载的文件数、字节数、速度和可用时的剩余时间估算。
蓝色进度条居中只显示数字和百分号（如 `50%`），表示各自方向的进度，
单方向 100% 不代表整轮同步成功。恢复偏移和复用的下载字节不计入本轮网络速度。
本轮摘要保留下载、上传、移动、删除、阻止数量及最近结束时间。
扫描及其他总量未知的阶段显示文字
提示，不显示容易被误认为已满的条纹进度条；整个任务成功结束后才显示完成的 100%。
**停止** 发出取消请求并等待清理；任务期间禁止登录、注销和更改设置，关闭窗口会
取消并回收任务。**日志** 页按时间、操作类型和级别显示应用事件，跨任务保留本次
运行的历史，最多 500 项，省略数量明确显示。级别筛选表示最低级别：选择“警告”
会显示警告、错误和严重日志，“所有级别”显示全部。英文同步日志标签缩短为 `Sync`。
关闭自动滚动后，新日志到达或旧历史淘汰时保持当前阅读条目；若该条目已被淘汰，
则停留在最早保留的日志。重新开启自动滚动会跳到最新条目。支持
复制当前显示的日志及清空；清空不会停止任务，切换页面／语言不会重启任务或清空历史。
后端日志按配置的日志级别显示，包括每次下载的文件路径和大小。
前后端共用跟踪、调试、信息、警告、错误、严重六个级别；成功事件使用信息级别，
不再单列“成功”。筛选不会启用低于配置日志阈值的日志。
后端原始详情保留，账号日志不加入授权链接或令牌。此页不读取配置的日志文件，
也不额外写入日志文件。
**启动监控**按已保存配置运行共享 Monitor worker；监控期间的**立即同步**请求
现有 Monitor，不另启同步引擎。GUI 还会发现外部 CLI／服务 Monitor，可请求同步
或经确认后停止；关闭 GUI 只停止自有任务，不停止外部 Monitor。尚不提供直接
启动／重启 systemd unit 的按钮。
详见[监控与所有权](docs/usage.zh-CN.md#gui-监控与外部任务所有权)。

GUI 自有监控同时显示会话累计及最近一轮统计：下载、上传、移动、删除按已报告的轮次摘要累加，
阻止项始终取最近一轮数量。新建 Monitor 任务才重置会话统计。
外部 Monitor 快照不提供实时传输进度或这些统计。

**文件**页显示应用启动以来已完成操作的本地时间、动作、路径、原路径和 Drive ID，
支持搜索。清空日志或重启 Monitor 不清除记录，退出应用后清空。
两端共用 `history.maximum_entries`，默认保留最近 **10000** 条，超限显示省略
数量；GUI 可在**设置／高级与日志**中修改。只列出当前进程完成的操作，不含外部
Monitor 历史、预览、阻止项和已是最新的文件复用。CLI 文本／JSON 实时输出；
TUI 显示最近记录，结束后输出保留列表。
详见[文件操作历史](docs/usage.zh-CN.md#文件操作历史)。

同步状态下方的**阻止项**列表从当前账号和 Drive 的数据库离线读取，无需连接 Graph。
预览、同步及 CLI Monitor 发现的阻止项统一展示，不再区分预览结果和已保存记录。
支持按路径／原因搜索、按原因代码筛选，选择项目后可查看和复制完整详情；**刷新**
重新读取数据库。清空日志不影响阻止项，任务失败、取消或重启程序也不会丢失记录。
只有后续检查确认问题已解决才删除对应记录，不会因为本轮未遇到就清空。
CLI 使用 `onedrive-cpp inspect blocked` 离线查询同一批记录，支持
`--reason CODE`、`--search TEXT` 筛选以及 `--output json` 自动化输出；
字段与 TUI 操作见[使用说明](docs/usage.zh-CN.md)。
预览允许保存阻止诊断，但仍不传输文件、不推进已应用游标、不提交普通文件快照。

详情同时展示同步引擎共用的处理策略。OneNote 等不支持格式采用不可修改的固定策略；
其他原因遵循各自的重新验证规则及既有冲突／删除配置，界面不提供通用解除或强制
绕过操作。

完整预览的远端元数据暂存在内存中；下一次预览／同步只检查预览之后的增量，
合并尚未执行的变更，并按当前本地文件重新生成计划。预览不会推进已应用的同步游标。
配置、账号、筛选规则或持久化状态变化会使缓存失效；游标过期、远端目录移动／删除
会回退到完整查询。存在待恢复记录时不缓存；取消或失败会清除缓存，实际同步会消费
缓存，重启 GUI 后不会保留。

CLI 和 GUI 的实际同步会在执行计划前，将独立的**待应用远端 delta 批次**保存到
SQLite。中断／重启后，下一次同步恢复原始变更，只从其获取游标检查后续增量，
再按当前本地状态重新生成计划；已完成文件和部分下载继续复用既有恢复机制。
远端计划提交时才推进已应用游标，并在同一事务中清除待应用批次。账号／目录／
Graph／认证／校验上下文、筛选或已应用基线不匹配时不复用；游标失效及目录结构
变化回退完整查询。预览不写入待应用批次。`state reset` 也会清除此元数据，
但保留文件恢复记录。

悬停在配置项、标签或浏览／覆盖控件上可查看简洁提示，高级项也有提示。
提示同时提供无障碍描述，切换语言立即更新，不改变草稿。

设置改版第一、二阶段已提供 **账号与目录 / 同步规则 / 传输 / 网络 / 监控 /
高级与日志** 六个子页，共用一个
草稿和全局保存、导入、重新加载按钮。保存使用核心 TOML 解析器验证，保留注释与
未编辑配置，备份原文件后原子替换。导入和重新加载替换草稿；切换子页或语言
保留草稿。更改目录不会迁移文件或凭据；设置在下次操作生效，登录或同步期间禁用设置。

已覆盖应用／租户／驱动器 ID 与目录；同步方向、删除／冲突／点文件策略、
选择性同步规则、文件大小限制及高级 `.nosync`／根文件／远程删除保护；上传下载
并发、单请求与总速率、顺序及高级分块／检查点／重试／下载校验。大小单位为字节，
速率为字节/秒（大小与速率的 `0` 表示不限，远程删除数量的 `0` 表示禁止删除）。
整数保持 TOML 非负有符号 64 位精度。

第二阶段增加代理 URL／绕过列表覆盖、用户名及密码／CA 文件引用、网络超时与
HTTP／IP 模式、Graph 限流、监控轮询／稳定等待／WebSocket 设置、文件历史上限、日志级别／文件、
文件权限及元数据模式。未勾选的代理覆盖项分别继承环境变量；显式空 URL 强制直连，
空绕过列表覆盖 `NO_PROXY`。不输入明文密码，编辑器不读取引用文件，运行时仍执行
文件安全检查。账号高级设置包含可信 HTTPS 认证／Graph 端点、权限范围（更改后
需重新登录）及必需挂载点，不迁移文件、凭据或状态。普通设置不暴露持久化试运行／
强制删除标志，也不编辑 CLI 主题。

`ONEDRIVE_BUILD_GUI` 默认为 `OFF`；无界面构建无需 Qt 或翻译工具。
维护和离屏验证方法见[GUI 本地化测试](docs/development.zh-CN.md#gui-本地化)。

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

用户服务在安装阶段生成，`ExecStart` 会匹配实际安装前缀，也支持
`cmake --install ... --prefix` 覆盖和 CPack 默认的 `/usr` 前缀，
不会写入 `DESTDIR` 暂存目录。自定义前缀不在 systemd 用户服务搜索路径内时，
以普通用户身份链接已安装的服务，再重新加载用户管理器：

```bash
systemctl --user link /opt/onedrive-cpp/lib/systemd/user/onedrive-cpp.service
systemctl --user daemon-reload
```

启用 `ONEDRIVE_BUILD_GUI=ON` 时，还会安装 `onedrive-cpp-gui`、
桌面应用入口和 hicolor 应用图标。CLI 和 GUI 一起安装，不生成独立 GUI 包。

### 快速生成 CPack 包

使用 CPack 快速生成未经过完整 Debian 策略检查的包。
在 Ubuntu 24.04 上构建 CLI/TUI 包：

```bash
cmake --preset release -DONEDRIVE_BUILD_GUI=OFF
cmake --build --preset release
ctest --preset release
```

在 Ubuntu 26.04 上，先按“可选桌面 GUI”一节安装 Qt 依赖，
然后构建 CLI/TUI 与 GUI 合包：

```bash
cmake --preset release -DONEDRIVE_BUILD_GUI=ON
cmake --build --preset release
ctest --preset release
```

完成对应构建后生成安装包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。CMake 会从 `/etc/os-release` 读取 `ID` 和
`VERSION_ID`，因此 Ubuntu 24.04 amd64 构建命名为：

```text
onedrive-cpp_<version>-<revision>~ubuntu24.04_amd64.deb
```

在跨发行版构建环境中，可在配置时显式覆盖检测结果：

```bash
cmake --preset release \
  -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04
```

Ubuntu 26.04 包仍必须在 Ubuntu 26.04 环境中构建和测试；仅修改后缀不会改变
二进制 ABI。

包修订号默认取 `debian/changelog` 最新条目中的修订号，不含发行版后缀。
仅为 CPack 显式递增修订号时可配置：

```bash
cmake --preset release -DONEDRIVE_PACKAGE_REVISION=2
```

将该选项设回空字符串即可重新跟随 changelog。CMake、`vcpkg.json` 和
changelog 的应用版本必须一致。CPack 在打包前再次核对源码版本，拒绝过期的
默认修订号或不一致的版本覆盖；修改发布元数据后应重新配置并编译。
版本格式为 `<应用版本>-<修订号>~<发行版>`，不使用 epoch。

本轮正式发布范围为两个发行版的 amd64。Ubuntu 26.04 包包含 Qt
运行依赖和平台插件；Ubuntu 24.04 包不包含 GUI，也不要求 Qt。
arm64 不在本轮发布验证范围内。

系统配置会登记为 Debian conffile，默认路径为
`/etc/onedrive-cpp/onedrive-cpp.toml`。安装包升级会保留本地修改，
或在随包默认配置变化时询问处理方式；普通卸载保留配置，
`apt purge onedrive-cpp` 才删除配置。无人值守升级若要求保留本地配置，
应显式选择 dpkg 的 `--force-confold` 策略，包括从未登记 conffile 的
旧 CPack 包升级。直接使用 `cmake --install` 不具备 dpkg 的配置保护。

### 准备离线构建用的发布源码包

在 Git 工作区中，联网准备发布源码包：

```bash
python3 tools/prepare_source.py
tar -xf build/source/onedrive-cpp-1.0.2.tar.xz -C build/source
cd build/source/onedrive-cpp-1.0.2
```

归档包含 Git 已跟踪文件的当前工作区内容，以及 `third_party/` 下经过
SHA256 校验的 FTXUI/Proxy 源码归档；不包含未跟踪文件或构建缓存。
正式发布应使用干净、已提交的工作区，第三方源码不提交到 Git。
下载缓存位于 `build/dependency-archives`；传入 `--archives-dir PATH --offline`
可完全离线准备源码，缓存缺失或损坏会明确失败。
归档时间戳取 `SOURCE_DATE_EPOCH`，未设置时取最近一次 Git 提交时间。

断网前先安装目标发行版的系统构建依赖。Debian 规则始终启用
`ONEDRIVE_OFFLINE_BUILD=ON`，随包归档缺失或损坏时直接报错，
不回退联网或选择已安装的 FTXUI/Proxy。使用准备好的源码手动构建
CMake/CPack 包时，也要在配置阶段传入 `-DONEDRIVE_OFFLINE_BUILD=ON`。
源码包不包含操作系统工具链或 Qt，仍需匹配的目标环境；仅此一步不保证
二进制安装包逐字节可复现。

### 构建正式 Debian 包

安装打包依赖：

```bash
sudo apt install -y build-essential devscripts debhelper cmake ninja-build python3 git \
  clang-20 libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

Ubuntu 26.04 还需安装 GUI 构建依赖：

```bash
sudo apt install -y qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-translations-l10n
```

在上文准备并解压的源码目录，使用 `nogui` 构建 profile 制作 Ubuntu 24.04 CLI/TUI 包，
不引入 Qt 构建或运行依赖：

```bash
source "$HOME/.profile"
chmod +x debian/rules
DEB_BUILD_PROFILES=nogui dpkg-buildpackage --build=binary --no-sign
```

在 Ubuntu 26.04 上，不启用 `nogui` profile，将 CLI/TUI 与 GUI
合并到同一个 `onedrive-cpp` 包：

```bash
source "$HOME/.profile"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` 会执行 CMake 配置、编译和测试。生成的 `.deb` 位于项目
目录的上一级，例如：

```text
../onedrive-cpp_1.0.2-1~ubuntu24.04_amd64.deb
```

Debian 规则启用 `ONEDRIVE_DEBIAN_PACKAGE_BUILD=ON`。在依赖准备或编译之前，
CMake 要求 changelog 的完整版本与应用版本、修订号及实际构建发行版匹配，
且 suite 与 `/etc/os-release` 的 `VERSION_CODENAME` 一致。
选择构建 profile 不会自动修改版本。仓库内 changelog 面向 Ubuntu 24.04
（`noble`）；在 Ubuntu 26.04（`resolute`）构建前应更新对应条目，例如：

```bash
dch --newversion 1.0.2-1~ubuntu26.04 --distribution resolute
```

Ubuntu 24.04 使用 `noble` 和 `ubuntu24.04` 后缀。仅修改打包流程时，
递增 Debian 修订号，不改变应用版本。普通开发和快速 CPack 构建允许
changelog 面向其他目标发行版，但仍要求上游应用版本一致。

安装并检查：

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
man onedrive-cpp
```

Ubuntu 26.04 的应用菜单还会提供 **OneDrive 客户端** 入口。
可运行 `onedrive-cpp-gui --version` 检查安装后的 GUI 版本。

如需检查 Debian 策略问题：

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```
