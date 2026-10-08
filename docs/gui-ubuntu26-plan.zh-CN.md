# Ubuntu 26 GUI 支持准备与实施计划

[文档中心](README.zh-CN.md)

## 目标

为 `onedrive-cpp` 增加 Ubuntu 26 桌面 GUI，同时保持现有 CLI、JSON 输出、FTXUI
界面和 systemd user service 的行为不变。GUI 应直接复用认证、Graph、SQLite、
同步和 Monitor 能力，不通过启动 CLI 子进程或解析终端输出来驱动业务。

首个版本优先提供可靠的账号登录、状态查看、手动同步、同步取消和错误展示，不在
初期同时引入托盘、多账号、完整文件浏览器等高复杂度功能。

## 当前基础

项目已有以下可复用边界：

- `RuntimeFactory` 负责创建 HTTP、认证、Token、Graph、数据库、Monitor 和
  Metrics 适配器。
- `ConsoleEvent` 已将消息、区段、Delta 进度、下载进度和阻塞项建模为强类型
  事件。
- `DeviceAuthClient` 已将请求设备代码和轮询 Token 分为独立操作。
- Graph、SQLite、同步引擎和 Monitor 已按模块组织，并有依赖注入和测试替身。

这些能力可以继续作为 GUI 的底层基础，但目前核心 target 和用户界面层仍有较强
耦合，需要先进行架构准备。

## 当前阻碍

以下记录最初评估时的架构；已完成的改造与剩余工作见 P0 清单。

### 核心 target 混合多层职责

`onedrive_core` 当前同时包含：

- Graph、HTTP、认证、存储、同步和 Monitor；
- CLI 参数解析和命令编排；
- Text、JSON 和 FTXUI backend；
- 终端能力探测；
- CLI11 和 FTXUI 依赖。

如果直接加入 Qt，GUI 将被迫链接不需要的 CLI/TUI 组件，核心变更也会触发更多
第三方代码重新编译和检查。

目标结构：

```text
onedrive_core
  Graph、HTTP、认证、存储、同步、Monitor、配置模型

onedrive_app
  用例编排、操作请求、结果、状态和进度事件

onedrive_cli
  CLI11、Text、JSON、FTXUI、终端探测

onedrive_gui
  Qt、窗口、视图模型和桌面集成
```

### 业务层依赖控制台概念

同步引擎、单文件下载和命令编排仍直接使用 `cli::Console`。GUI 虽然可以临时实现
另一个 `ConsoleBackend`，但这会让业务层继续依赖控制台命名和同步确认模型。

应将现有强类型事件迁移为 UI 中立的应用事件，并由 CLI 和 GUI 分别提供 adapter。
迁移期间应保持现有事件字段和 CLI 输出不变，避免不必要的行为变化。

### 应用入口由 CLI 驱动

当前 `Application::run()` 同时负责参数解析、配置加载、UI 选择、日志初始化、
命令执行和错误输出。GUI 不应构造命令行参数调用该入口。

需要提取可由不同前端调用的应用服务，例如：

```cpp
class OneDriveService {
public:
    LoginResult login(
        const LoginRequest& request,
        OperationObserver& observer,
        std::stop_token stop_token
    );

    SyncResult synchronize(
        const SyncRequest& request,
        OperationObserver& observer,
        std::stop_token stop_token
    );

    StatusResult status(const StatusRequest& request);
};
```

CLI 的 `Application::run()` 和 GUI worker 应调用同一应用服务，测试也应直接验证
该服务，而不是通过进程输出间接验证业务。

### 长时间操作缺少统一 GUI 取消模型

网络请求、设备代码轮询、扫描、同步和 Monitor 都不能在 Qt 主线程执行。关闭
窗口、停止同步和注销账号时，操作必须能够真正终止，而不是只隐藏进度界面。

应用服务应统一接受 `std::stop_token`，并明确以下状态：

```text
idle
authenticating
ready
syncing
watching
stopping
failed
```

状态由应用层维护，GUI 只负责展示，不应根据零散进度事件自行推断。

## 技术方案

### GUI 框架

首版采用 Qt 6 Widgets：

- 与现有 C++ 和 CMake 工程直接集成；
- 原生支持 Ubuntu 的 Wayland 和 X11；
- 适合设置、账号、状态、日志和同步进度类桌面界面；
- 不引入 Node.js、Chromium 或 WebView 打包链。

GUI 构建必须可选，headless 环境不应需要安装 Qt：

```cmake
option(ONEDRIVE_BUILD_GUI "Build the Qt desktop application" OFF)

if(ONEDRIVE_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

    add_executable(onedrive-cpp-gui ...)
    target_link_libraries(onedrive-cpp-gui
        PRIVATE
            onedrive_app
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
endif()
```

如果后续需要大量动画、触摸交互或移动端支持，再单独评估 Qt Quick/QML。

### 线程与事件

- Qt 主线程只执行界面逻辑。
- 应用服务在受控 worker 中执行。
- worker 发布强类型应用事件。
- Qt adapter 使用 queued connection 将事件转发到主线程。
- worker 生命周期由明确的操作对象管理，不允许 detached thread。
- 所有停止路径都汇入同一取消机制并等待资源安全释放。

### 后台服务职责

项目已有 systemd user service。首版不应再创建一套独立的常驻同步机制。

MVP 可以让 GUI 进程直接运行同步，但必须：

- 复用现有锁机制避免 GUI、CLI 和 systemd service 同时操作同一账号；
- 明确提示关闭 GUI 会停止由 GUI 启动的同步；
- 不声称 GUI 已提供真正的后台同步。

长期方案是由 systemd user service 持有同步引擎和数据库，GUI 通过 DBus 或受控
Unix socket 查询状态和发送控制请求。IPC 协议应作为独立阶段设计，不能用解析
日志或模拟 CLI 输入代替。

### Token 存储

MVP 保持现有文件 Token Store，以保证 GUI、CLI 和 systemd 环境兼容。正式桌面
版本再评估 Secret Service/libsecret，并设计：

- 现有文件 Token 的安全迁移；
- GNOME Keyring 不可用时的明确错误；
- headless 用户继续使用文件存储的策略；
- GUI 与 systemd service 访问同一凭据的方式。

## Ubuntu 26 构建与测试准备

### 构建矩阵

建立两个独立配置：

1. Headless CLI：
   - `ONEDRIVE_BUILD_GUI=OFF`；
   - 不安装或查找 Qt；
   - 验证服务器和 systemd 使用场景。
2. Desktop GUI：
   - `ONEDRIVE_BUILD_GUI=ON`；
   - 使用 Ubuntu 26 仓库提供的 Qt 6 开发包；
   - 验证 Wayland 和 X11/XWayland。

### GUI 测试

至少覆盖：

- 应用服务的无界面单元测试；
- Qt `offscreen` 平台下的视图模型和组件测试；
- Wayland 会话启动；
- X11/XWayland 会话启动；
- 没有 `DISPLAY` 和 `WAYLAND_DISPLAY` 时 CLI 仍正常；
- GUI 在没有图形环境时给出清晰错误，而不是崩溃；
- 登录和同步期间停止操作；
- 关闭窗口时 worker、数据库和网络资源安全释放；
- CLI、GUI 和 service 的多实例锁冲突。

第三方 FTXUI 和 Qt 生成代码不应进入项目 clang-tidy 检查范围。核心、应用、CLI
和 GUI target 应能分别运行针对性 lint。

## 安装与打包

现有安装规则只覆盖 CLI、systemd user service、文档、man page 和 shell 补全。
GUI 包还需要：

- `share/applications` 下的 `.desktop` 文件；
- `share/icons/hicolor` 下的标准尺寸图标；
- `share/metainfo` 下的 AppStream metadata；
- GUI 可执行文件及所需 Qt 运行时依赖；
- 应用名称、菜单分类和本地化名称；
- AppStream 和 desktop 文件验证；
- 明确 GUI 与 CLI 是同包还是拆包。

已有 PNG 和 SVG logo 可以复用，但桌面图标应安装到标准目录，不能只安装到文档
资源目录。

## 分阶段任务

### P0：GUI-ready 架构

- [x] 将 `onedrive_core` 拆分为核心、应用和 CLI target。
- [x] 将强类型界面事件迁移为 UI 中立的应用事件。
- [x] 消除同步和下载代码对 `cli::Console` 的直接依赖。
- [ ] 提取登录、状态、同步和监控应用服务。
- [ ] 为长时间操作增加统一的 `std::stop_token` 取消语义。
- [ ] 建立明确的应用操作状态机。
- [ ] 让 CLI、JSON 和 FTXUI 通过 adapter 使用新的应用服务。
- [ ] 保持现有 CLI 输出、退出码、确认行为和配置兼容。
- [ ] 为新边界补充应用服务和取消路径测试。
- [ ] 将 clang-tidy 按 target 运行并排除第三方生成代码。

当前已完成三个 target 的物理拆分及 UI 中立事件迁移：

- `events::Event` 和线程安全的 `events::Observer` 位于核心层，不引入新的
  target，也不依赖 `onedrive_app`。
- `cli::Console` 继承观察者接口，现有 CLI 事件名称保留兼容别名。
- `SyncEngine` 必须显式传入非空观察者；CLI 继续传入 Console，其他前端可注入
  自己的观察者，不会隐式启动终端界面。
- 终端配置类型和解析迁入 `config`；TOML 配置键及取值保持不变。
- `onedrive_core` 已解除对 CLI、CLI11 和 FTXUI 的链接依赖。
- 新增仅链接核心的观察者测试，覆盖全部事件载荷、并发串行化、异常传播，以及
  通过非 Console 观察者完成真实本地文件写入的模拟 Graph 同步。

应用可执行文件仍只直接链接 `onedrive_app`。当前 `onedrive_app` 同时依赖
`onedrive_core` 和 `onedrive_cli`，而 `onedrive_cli` 依赖 `onedrive_core`。
下一步提取应用服务并移动 CLI 参数解析和生命周期职责，才能形成最终的
`onedrive_cli -> onedrive_app -> onedrive_core` 方向。异步取消和确认请求的 GUI
适配尚未实现，本阶段不代表完整 P0 已完成。

### P1：最小 Qt GUI

- [ ] 增加默认关闭的 `ONEDRIVE_BUILD_GUI` CMake 选项。
- [ ] 增加独立 `onedrive-cpp-gui` target。
- [ ] 建立主窗口和应用状态视图模型。
- [ ] 显示当前配置和账号状态。
- [ ] 实现设备代码登录，并调用系统浏览器打开验证地址。
- [ ] 实现一次手动同步的启动和停止。
- [ ] 展示消息、Delta、下载进度、阻塞项和最终错误。
- [ ] 提供打开同步目录和日志目录的操作。
- [ ] 验证关闭窗口时取消并回收所有 worker。

### P2：Ubuntu 26 桌面交付

- [ ] 增加 `.desktop`、标准图标和 AppStream metadata。
- [ ] 增加 GUI 安装和 Debian/CPack 依赖规则。
- [ ] 决定 CLI 与 GUI 的同包或拆包策略。
- [ ] 建立 Ubuntu 26 headless 和 desktop 构建矩阵。
- [ ] 增加 Wayland、X11/XWayland 和 offscreen 测试。
- [ ] 验证无图形环境和缺失 Qt 依赖时的错误行为。
- [ ] 编写 GUI 使用、故障排查和打包文档。

### P3：桌面增强

- [ ] 设计并实现 GUI 与 systemd user service 的稳定 IPC。
- [ ] 增加服务状态、启动、停止和重启控制。
- [ ] 增加桌面通知。
- [ ] 增加托盘图标和开机启动策略。
- [ ] 评估并接入 Secret Service/libsecret。
- [ ] 设计文件 Token 的安全迁移和 headless fallback。
- [ ] 在应用服务支持后增加多账号管理。

## 首个实施里程碑

第一个里程碑只完成 P0，不立即添加 Qt 界面。验收标准：

1. `onedrive_core` 不再编译 CLI11、FTXUI 或终端代码。
2. 同步和认证用例可以不经过 `Application::run()` 调用。
3. 业务层不再依赖 `cli::Console`。
4. 长时间应用服务接受取消请求并有测试覆盖。
5. 现有 CLI、JSON、FTXUI 测试继续通过。
6. headless 构建不引入 Qt。

完成该里程碑后，再建立最小 Qt GUI，能够显著降低线程、生命周期、测试和打包
方面的返工风险。

## 深度 lint 记录

`build/tidy-runs/20261008-204540/lint-deep.log` 没有记录 clang-tidy 或静态分析
诊断。该次构建在完成 81/339 个步骤后显示：

```text
ninja: build stopped: interrupted by user.
```

因此这次运行既不是 Ubuntu 26 兼容性失败，也不能视为 lint 通过。它说明当前
全量检查会先构建大量 FTXUI 源码，并进一步支持按 target 拆分构建和 lint 的必要性。
