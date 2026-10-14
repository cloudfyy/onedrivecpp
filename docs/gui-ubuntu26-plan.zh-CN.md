# Ubuntu 26 GUI 支持准备与实施计划

[文档中心](README.zh-CN.md)

## 目标

为 `onedrive-cpp` 增加 Ubuntu 26 桌面 GUI，同时保持现有 CLI、JSON 输出、FTXUI
界面和 systemd user service 的行为不变。GUI 应直接复用认证、Graph、SQLite、
同步和 Monitor 能力，不通过启动 CLI 子进程或解析终端输出来驱动业务。

首个版本优先提供可靠的账号登录、状态查看、手动同步、同步取消和错误展示，不在
初期同时引入托盘、多账号、完整文件浏览器等高复杂度功能。

## 当前基础

### 设置改版第一阶段（已实现）

保留概览／设置主导航，设置内提供 **账号与目录 / 同步规则 / 传输** 三个子页，
共用草稿与全局保存、导入、重新加载。切换语言和子页保留编辑；导入与重新加载
替换草稿；登录期间禁用设置。保存复用核心验证器与基于 TOML 源位置的类型化补丁，
只改编辑键、保留注释和无关配置，验证后备份并原子替换。缺失配置可在 GUI 保存
应用 ID 后登录；损坏配置不能被默认值覆盖，可导入或修复后重新加载。

- 账号与目录：应用 ID、租户 ID、同步目录、状态目录、驱动器 ID，目录浏览与
  不迁移文件／凭据警告。
- 同步规则：方向、删除、冲突、选择性规则文件、点文件、最大文件字节数；高级
  `.nosync`、根文件和最大远程删除数量。
- 传输：上传／下载并发、单请求／总速率、顺序；高级上传分块、下载阈值、
  检查点、重试与下载校验。数字保持 TOML 非负 int64 精度，单位和零值语义明确。

### 设置改版第二阶段（已实现）

已补齐 **网络 / 监控 / 高级与日志**，设置共六个子页，继续共用草稿和全局
保存／导入／重新加载，语言切换不改变枚举值或未保存编辑。

- 网络：独立的可选代理 URL 和绕过列表覆盖、用户名、密码文件引用、认证模式、
  HTTPS 代理 CA 文件；连接／请求／停滞超时、最低速率、HTTP／IP 版本；
  高级 Graph 限流重试与初始／最大延迟。
- 监控：WebSocket 开关、轮询秒数、本地变更稳定等待毫秒数；高级 WebSocket
  请求／连接超时、提前续订与初始／最大退避。
- 高级与日志：日志级别及可选文件、同步文件权限、文件系统元数据；配置路径和
  CLI `doctor` 诊断提示。
- 账号高级：可信 HTTPS 身份验证／Graph 端点、权限范围数组和可选必需挂载点。
  明确提示端点／权限范围更改需重新登录、挂载要求不代表目录迁移。

代理未设置与显式空覆盖分别表示继承环境和强制直连／空绕过列表；空代理表与
缺失表不会被未编辑保存归一化。密码只能引用私有文件，不在 GUI 输入秘密或放入
URL；编辑器不读取文件内容，运行时安全检查保持有效。核心类型化补丁支持数组、
嵌套限流表及代理表移除，仍保留注释和无关键。保存／导入先通过核心验证与 GUI
端点安全校验，错误不会写入或创建备份。

不暴露普通持久化试运行／强制删除标志或 CLI 主题。新配置仅影响下次操作，
不修改活动 worker。GUI 手动同步已实现；监控任务、目录迁移和完整后台服务集成仍暂缓。

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
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

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

GUI 使用 Qt 6.8 及以上版本，以便将应用颜色方案留给桌面平台并跟随系统主题。
Ubuntu 26 的 `qt6-base-dev` 提供所需的 Qt Core、Gui 和 Widgets 开发组件。

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
- [x] 提取认证、配额和同步状态查询应用接口。
- [x] 提取手动同步应用服务。
- [x] 提取 Monitor 应用服务。
- [x] 为认证、同步、Monitor、Delta、下载和上传主路径贯通 `std::stop_token`。
- [x] 将取消贯通到启动恢复、远端移动/删除及目录和路径查询 Graph 请求。
- [x] 定义长操作的 GUI 状态转换。
- [x] 建立明确的应用操作状态机。
- [x] 让 CLI、JSON 和 FTXUI 通过 adapter 使用新的应用服务。
- [x] 保持现有 CLI 输出、退出码、确认行为和配置兼容。
- [x] 为认证、状态查询及已贯通取消路径补充测试。
- [x] 为手动同步应用服务边界补充测试。
- [x] 为 Monitor 应用服务边界补充测试。
- [ ] 将 clang-tidy 按 target 运行并排除第三方生成代码（本阶段跳过，耗时较长）。

当前已完成三个 target 的物理拆分及 UI 中立事件迁移：

- UI 源码和公共头文件已按 `ui/common`、`ui/cli`、`ui/gui` 分组。共用事件接口和
  实现位于 `common`，终端后端以及 CLI 编排和格式化位于 `cli`，Qt 的 `gui`
  目录已预留。运行时工厂、结构化查询和预检仍在应用层；本次重组不引入 Qt。
- `events::Event` 和线程安全的 `events::Observer` 位于核心层，不引入新的
  target，也不依赖 `onedrive_app`。
- `cli::Console` 继承观察者接口，现有 CLI 事件名称保留兼容别名。
- `SyncEngine` 必须显式传入非空观察者；CLI 继续传入 Console，其他前端可注入
  自己的观察者，不会隐式启动终端界面。
- 终端配置类型和解析迁入 `config`；TOML 配置键及取值保持不变。
- `onedrive_core` 已解除对 CLI、CLI11 和 FTXUI 的链接依赖。
- CLI 参数解析、命令分发、展示编排及 `Application::run()` 生命周期已移入
  `onedrive_cli`；依赖方向为 `onedrive_cli -> onedrive_app -> onedrive_core`，
  `onedrive_app` 不再反向依赖 CLI、CLI11 或 FTXUI。
- CLI、JSON、FTXUI 命令流程及配置相关回归测试在调整 target 依赖后通过；覆盖
  参数帮助、命令流程、确认/退出行为和配置解析。
- 新增仅链接核心的观察者测试，覆盖全部事件载荷、并发串行化、异常传播，以及
  通过非 Console 观察者完成真实本地文件写入的模拟 Graph 同步。
- 配额和同步状态已提取为拥有数据的公共查询快照。CLI 继续生成相同 section 和
  JSON 字段，Qt 可以直接读取原始字节数、枚举、最近运行结果和数据库摘要。
- 认证已提取为 `authenticate_account`，前端通过结构化回调读取验证码、验证网址、
  消息和有效期。默认轮询等待及认证、身份和头像 HTTP 请求支持 stop token；
  取消在账号激活前生效，持久化开始后允许其完成。CLI 授权文本与退出码保持不变。
- 同步入口和 Monitor 回调现接受 stop token，并将停止请求传递给 Delta OAuth 刷新、
  分页请求、下载与上传 worker，以及启动恢复中的 Graph 操作。远端移动、删除、目录
  创建和路径查询也将 token 传入 HTTP 请求与重试循环；取消映射为正常的
  `RequestCancelledError`。取消返回 130，指标单独记录为 `cancelled`；同步计划仅在
  安全边界响应取消，worker 会 join，未完成 Delta 不推进本地游标，未完成的移动、
  删除和上传 journal 保留用于恢复。
- 取消覆盖包含同步启动与执行边界、Delta/OAuth、Graph 上传各阶段、下载 checkpoint、
  启动恢复、远端移动/删除的 HTTP 请求与 journal 保留，以及 Monitor 停止传播；相关
  测试覆盖 Graph 请求 token 和取消时保留移动/删除 journal。Graph 测试还覆盖有效
  Access Token 跨操作复用、进入一分钟安全余量时提前刷新及 Refresh Token 轮换持久化。
  同时覆盖 Graph 客户端无效配置和依赖拒绝、身份查询各阶段的取消、头像服务异常，
  以及 Refresh Token 失效时的应用层错误映射。Monitor stop token 与进程信号、键盘退出
  可同时生效。
- 手动同步已提取为 `app::synchronize_account`：它负责创建 Graph、状态存储和指标适配器，
  解析账号对应的同步目录，并把 UI 中立观察者与取消 token 传给同步引擎。CLI 手动同步
  已改为调用该服务；应用服务测试覆盖 dry-run 事件、依赖装配和预先取消。
- Monitor 已提取为 `app::monitor_account`，由应用层装配共享的 Graph、状态存储、指标
  和同步回调；服务支持 stop token，并通过 UI 中立事件报告 Monitor 状态。CLI 仍保留
  原有 TUI 键盘退出选项，Monitor 同时接受 stop token、SIGINT/SIGTERM 和键盘退出。
- 应用观察者现收到类型化操作状态事件，状态机验证认证、同步和 Monitor 的合法迁移：
  认证为 `idle -> authenticating -> ready/idle/failed`，同步为
  `idle -> syncing -> ready/failed` 或经 `stopping` 后结束，Monitor 为
  `idle -> watching -> stopping -> ready` 或以 `failed` 结束。取消与完成不会报告为
  失败；CLI 文本、JSON 和 FTXUI adapter 忽略该状态事件，输出行为保持不变。

CLI 参数解析和生命周期编排现归属 `onedrive_cli`；目标依赖方向为
`onedrive_cli -> onedrive_app -> onedrive_core`。应用服务不再依赖 CLI 或终端库；
Qt 登录和手动同步现已适配异步取消；确认请求适配仍待实现。

认证、配额、状态查询、手动同步和 Monitor 已有可复用应用接口。底层恢复和远端
移动/删除操作现可响应取消；Qt 前端确认适配尚未完成。

### P1：最小 Qt GUI

- [x] 增加默认关闭的 `ONEDRIVE_BUILD_GUI` CMake 选项；关闭时不查找 Qt。
- [x] 增加独立 `onedrive-cpp-gui` target 和最小 Qt Widgets 窗口入口。
- [x] 建立主窗口和应用状态视图模型。
- [x] 显示配置路径、同步目录、状态目录和本地账号凭据状态。
- [x] 增加跟随系统浅色/深色方案的 Qt Widgets 外观。
- [x] 增加同步目录和状态目录设置、目录选择、TOML 导入预览及安全保存。
- [x] 完成设置第一、二阶段：六个子页、类型化增量保存、双语与离屏回归。
- [x] 实现系统浏览器授权码 + PKCE 登录、localhost 随机端口回调和取消；CLI 保留设备代码登录。
- [x] 实现一次手动同步的启动和停止。
- [x] 展示消息、Delta、下载进度、阻塞项和最终错误。
- [ ] 提供打开同步目录和日志目录的操作。
- [x] 验证关闭窗口时取消并回收所有 worker。

GUI 窗口展示当前配置路径、同步/状态目录和本地凭据状态；启动时不访问网络，也不
将“本地存在凭据”误报为远端登录有效。设置按编辑键更新配置，其他 TOML 内容
及注释保留；保存前校验配置和目录关系、以原子方式写入并备份旧配置。导入会先显示
同步目录、状态目录、同步模式和删除策略预览；导入的相对文件引用按活动配置目录
解析。GUI 通过 XDG Desktop Portal 获取系统颜色方案并监听变化，明确应用浅色或深色
调色板；门户不可用时保留 Qt 平台外观。
配置或安全状态读取失败时显示错误对话框。登录使用系统浏览器、随机 state、
PKCE S256 和仅回环可达的随机端口；需在 Microsoft 应用注册中增加移动/桌面平台
`http://localhost` 回调，详见 [认证文档](authentication.zh-CN.md)。
等待回调、令牌交换和账号激活运行在后台线程，支持取消和关闭时回收。
PKCE 会话已抽为 Core `AuthCodeSession` 状态机，Qt 仅承担浏览器及 HTTP 回调适配；
GUI 和 CLI 共用账号 refresh token，并复用状态目录运行锁防止并发登录、刷新或注销。
授权参数编码和认证公共条件复用核心工具；PKCE 复用 SHA-256 二进制摘要及
Base64 URL-safe 编码，下载完整性校验仍使用标准 Base64（保留填充）。
自动测试使用本地回调和模拟传输，不访问 Microsoft；真实桌面和账号登录仍需验收。
主界面现分为概览、同步、日志、设置四页。概览显示账号、目录及任务摘要，
同步页提供预览变更、立即同步和停止，详情统一从日志页查看，后台 worker 直接调用
`app::synchronize_account`，不启动 CLI 子进程。读取已保存配置，保留设置草稿，
遵循持久化 dry-run 和远端删除保护；复用运行前检查、运行锁与日志配置。
预览保留未经筛选／增强的远端变更和独立游标，不推进已应用游标；下一次任务只查
后续增量，按 ID 合并后重建本地计划。配置来源／账号／筛选／持久化状态变化使缓存
失效，待恢复记录禁止缓存；目录移动／删除或过期游标回退完整查询，取消／失败及
实际同步清除缓存。缓存只保留在本次 GUI 运行的内存中。
强类型事件经有界队列送到 GUI 线程，连续进度合并，消息历史最多保留 500 项，
省略数量明确可见。日志跨任务保留本次运行的应用事件，包含时间、操作类型和级别，
支持级别筛选、自动滚动、复制及清空；不读取日志文件，不记录授权链接或令牌。
同步页显示扫描、下载百分比、速度、剩余时间及简短结果；详细计划和运行错误放入日志页。
状态下方提供统一阻止项列表，从当前账号／Drive 数据库离线读取，支持搜索、原因筛选
及可复制详情。预览、同步及 CLI Monitor 发现即落库；不依赖有界日志，不分预览／已保存。
预览仅允许写诊断，不推进游标、提交普通快照或修改同步文件；失败、取消和完整刷新
不清空记录，确认解决后才移除。后台查询合并刷新请求，丢弃旧配置／账号的过期结果，
查询失败明确展示错误；清空日志不影响记录。GUI 启动、任务发现／结束及账号／配置变化
会重新读取，CLI Monitor 运行时可手动刷新查看最新记录。
处理规则已抽为 Core 的共享策略接口与不可变注册表，多个原因复用远端／本地策略；
GUI 详情从同一策略读取能力说明，不自行判断原因前缀。OneNote 固定跳过并保护副本，
不可配置；其他原因区分冲突配置、资源恢复和删除保护，未知原因保守保留。
待恢复任务也接受策略检查，不能通过 journal 绕过固定限制。当前不增加策略编辑功能。
概览不提供同步按钮。未知总量阶段显示文字说明，不用容易被误读为已满的条纹条；
下载百分比明确标注，扫描完成和终端换行事件不会使阶段卡住或重置下载显示，
整个任务成功结束后才显示完成的 100%。
上传现已贯通共享进度事件，GUI／CLI／TUI／JSON 使用同一份传输事实及速度统计。
恢复偏移和复用文件不计入本轮速度；GUI 分别保留上传／下载信息、方向、单次运行方式、
本轮摘要及最近结束时间，上传 100% 不会提前结束任务。操作及历史支持即时切换语言。
同步期间禁用账号和设置操作；停止和关闭均请求取消，并等线程与运行锁安全释放，
晚到的进度不会覆盖停止状态。身份发现与令牌刷新也接收同一 stop token。
离屏测试覆盖真实应用服务的模拟预览／下载、持久化 dry-run、并发锁、进度及事件
上限、失败与重启、语言切换、操作互斥和关闭回收；测试不访问 Microsoft。

配置导入复用核心 `Config::load_from_string` 与共享配置目录关系校验；文件内容通过
通用受限读取工具加载，保留 1 MiB 配置上限及单硬链接/无符号链接要求，配置写入继续
复用原子文件工具。基本路径更新保留注释，并在修改后进行一次完整配置校验。

### P1 后续 TODO：双向同步 UI 与跨进程任务协调

本节按“上传事件与展示、运行互斥、Monitor
控制接口与 GUI 接入”的顺序逐步交付；CLI、GUI 和 systemd service 共用应用层
协调机制，不各自维护一套锁或启动独立同步引擎。

#### 上传进度与同步状态

- [x] 补齐 UI 中立的上传进度事件，提供文件数、字节数、速度及可用时的剩余时间；
  不解析日志文本，不把恢复或历史传输字节计入本轮网络速度，保持现有 CLI/JSON
  输出兼容。
- [ ] 将同步页改为同步状态页，显示同步方向（双向／仅上传／仅下载）、运行方式
  （单次／自动监控），区分扫描、下载、上传、应用变更、等待、停止和最终结果。
  已完成单次任务页面和 CLI Monitor 状态；GUI 自动监控显示随后续 Monitor 接入交付。
- [x] 分别展示上传与下载进度；未知总量使用文字，不生成虚假百分比；单方向
  100% 不代表整轮成功。保留本轮下载、上传、移动、删除、阻止数量及最后完成时间。
- [x] 分离任务生命周期和本轮同步阶段；现有 CLI Monitor 等待时显示正在监控，有阻止项时
  保留“上轮同步不完整”，不能在每轮结束后将监控任务误报为已退出。
  GUI 监控控制及 IPC 仍按后续阶段实施。

#### CLI／GUI／service 运行互斥

当前 CLI 的同步与 Monitor、GUI 手动同步已有同一状态目录的运行锁。以下为
补强工作，不代表已有锁失效或目前允许同一状态目录并发执行。

- [x] 保证所有同步入口共用运行预检；Monitor 在整个生命周期持有状态目录锁，
  包括等待期间，内部同步轮次不重复获取锁。停止请求必须等待清理、worker 退出
  和锁释放，期间不能启动新任务；设置和账号修改仍与活动任务互斥。
- [x] 补齐同步目录占用检查，阻止不同状态目录操作相同或父子重叠的本地同步目录。
  在安全解析实际账号／Drive 目录后处理路径别名、符号链接和文件系统身份；
  检查与登记由当前有效 UID 的私有 `/tmp` 登记目录和 OS 锁原子协调；
  CLI、GUI、同用户 service 须共享该本地命名空间，不覆盖跨用户／跨主机。
- [x] 提供结构化占用错误和可读的持有者信息（PID、操作、启动时间、目录）；
  OS 锁是占用判据，不能仅依赖 PID 文件或删除锁文件解锁。进程崩溃后应可安全
  再次启动，锁信息不包含认证秘密。

#### Monitor 控制接口与 GUI 接入

依赖上述运行互斥和任务状态模型。第一版保持显式控制，不让普通同步命令自动
转发、隐式排队或接管其他进程；暂停监控暂不纳入范围。

- [ ] 增加受控本地 IPC，优先评估私有状态目录内的 Unix domain socket；
  验证连接者身份，提供状态查询、请求同步、请求停止。协议不接受任意配置或
  目录，Monitor 只操作启动时确定的任务；处理协议版本、不响应及陈旧端点。
- [ ] 增加显式 CLI 手动触发入口，具体名称实施时确定。普通同步命令遇到
  Monitor 占用时明确报忙并提示触发方式；第二个 Monitor 不能并发启动。
  区分请求已接受／已合并和同步已完成，不能返回成功形状的实际执行结果。
- [ ] 将手动请求、本地变化和远端通知交给同一个串行调度器；等待时立即触发，
  执行中重复请求合并为一次后续检查，期间新增变化不能丢失；停止中拒绝新请求。
- [ ] GUI 接入同一 Monitor 服务和控制接口：单次任务期间禁止启动监控，
  监控等待时“立即同步”只请求现有 Monitor，执行时禁用重复触发。提供停止监控，
  明确关闭窗口对 GUI 自有任务和外部 service 的不同影响，不误停外部服务。

#### 验收与回归

- [ ] 使用真实子进程验证同步＋同步、同步＋Monitor、Monitor＋Monitor、
  CLI＋GUI／service 的互斥，以及不同状态目录、相同／重叠同步目录、路径别名、
  并发启动竞争和进程崩溃释放锁；不能仅靠同进程替身或按钮禁用验证。
- [ ] 验证 IPC 身份与请求校验、重复请求合并、执行中新变化、停止期间触发、
  请求与完成结果区分，以及失败／阻止后按既有策略继续监控或明确退出。
- [ ] 增加双语 GUI 与 CLI/JSON 回归，覆盖上传／下载阶段、未知总量、速度与
  计数、完成语义、监控等待、取消及关闭；更新相关使用与故障排查文档。

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
