# 安装包制作流程改进

## 待办事项

- [x] P0 明确安装包发布范围
  - 确定 Ubuntu 版本和 CPU 架构。
  - 明确 CLI/TUI 与可选 GUI 的包划分、包名及升级兼容策略。
- [x] P1 补齐 GUI 打包链路
  - 完善 GUI 程序、桌面入口和标准图标的安装规则。
  - 补齐 Debian GUI 构建开关及 Qt 构建依赖，保持纯 CLI 构建不依赖 Qt。
  - 检查实际 DEB 内容。
  - 已补齐条件安装、桌面入口、hicolor 图标和 Qt 平台插件运行依赖。
    Debian 默认构建 GUI 合包，Ubuntu 24.04 使用 nogui profile。
  - 已在 Ubuntu 26.04 验证 GUI 与禁用 Qt 的 CLI 构建、安装规则、
    两种 CPack DEB 内容及程序启动参数，并验证 Debian 依赖元数据生成。
  - Ubuntu 24.04 目标环境构建及 dpkg-buildpackage 端到端验证仍属于发布门禁；
    当前宿主机缺少 debhelper 和部分 Qt 构建工具，未完成原生打包验证。
- [ ] P1 保护 CPack 系统配置升级
  - 为系统配置声明 conffiles，验证升级保留用户修改。
  - 核对 Debian 原生包的配置文件处理。
  - 隔离安装测试已复现：升级覆盖本地配置修改，普通 remove 删除修改后的
    系统配置。两项失败均对应当前 CPack 包缺少 conffiles 元数据。
- [x] P1 执行当前安装包的隔离生命周期检查
  - Ubuntu 26.04 上使用非特权 Bubblewrap、临时写时复制目录及独立 dpkg
    数据库执行真实安装、包修订版升级、remove 和 purge。
  - 共 19 项检查，17 项通过、2 项配置保留检查失败，待配置保护项修复。
  - 已验证 CLI-only 包升级到 GUI 合包、安装后程序启动参数、桌面入口
    offscreen 启动、GUI 文件和 Qt 依赖、用户服务离线校验及 enable/disable。
  - 升级测试使用相同上游应用版本的包修订版 -1 到测试修订版 -2，
    不代表更早应用版本的数据迁移验证。
  - 测试继承宿主机已安装依赖，不是干净发行版环境；
    dpkg 使用 --no-triggers，尚未验证触发器及桌面缓存更新。
  - 未覆盖真实 X11/Wayland 显示、systemd 实际启动和账户同步、
    Ubuntu 24.04 环境及 Debian 原生构建。宿主机对应安装文件未改变。
  - 可重跑脚本、结果 JSON 和日志保存在当前会话 files/package-lifecycle/。
- [ ] P1 完善干净离线构建依赖
  - 约束 FTXUI 兼容版本，明确 FTXUI 和 Proxy 的来源及离线供应方式。
  - 完善 Debian 构建依赖和第三方版权声明。
  - 在干净目标环境验证构建不依赖宿主机缓存。
- [ ] P2 统一安装前缀和服务路径
  - 根据安装前缀生成 systemd 用户服务的 ExecStart。
  - 验证 /usr、/usr/local 和自定义前缀。
- [ ] P2 校验应用和发行版包版本
  - 检查 vcpkg.json、Debian changelog、CPack 版本及目标发行版的一致性。
  - 支持合法 Debian revision，不一致时显式失败。
- [ ] P2 修正发行包元数据
  - 修正项目来源、维护者占位信息和过时描述。
  - 为 CPack 包补齐许可证及版权文件。
- [ ] P2 建立隔离构建与发布门禁
  - 以 Debian 原生打包为正式主线，CPack 为快速试用入口。
  - 按目标平台建立容器或 chroot 干净构建。
  - 加入编译测试、包内容断言、版本检查、Lintian、安装启动、
    配置保留升级及卸载检查。
  - 验证后发布产物与校验和。
- [ ] P2 更新中英文安装包制作文档
  - 说明构建入口、目标环境、依赖准备、版本规则和配置升级行为。
  - 区分正式 Debian 包与 CPack 试用包。

## 依赖关系

- GUI 打包、构建依赖和版本方案依赖发布范围确定。
- 发布门禁依赖 GUI、配置保护、构建依赖、服务路径、版本和元数据工作完成。
- 制作文档在发布流程确定并验证后同步。

## 发布范围决策

已确认的目标范围如下，实际发布仍须通过对应平台的构建与安装门禁：

| 目标发行版 | 架构 | 包名 | 包含功能 |
|---|---|---|---|
| Ubuntu 24.04 | amd64 | onedrive-cpp | CLI/TUI |
| Ubuntu 26.04 | amd64 | onedrive-cpp | CLI/TUI 和 GUI |

- 采用合包方案，不新增独立的 onedrive-cpp-gui 二进制包。
  Ubuntu 26.04 的 onedrive-cpp 包同时安装 CLI 和 GUI 可执行文件。
- GUI 仅在 Ubuntu 26.04 发布，使用 Qt 6.8 或更高版本。
  Ubuntu 24.04 本轮保持纯 CLI/TUI，不引入额外 Qt 来源。
- Ubuntu 26.04 安装包将引入 GUI 所需的 Qt 运行依赖；
  本轮不提供该发行版的独立纯 CLI 包。
- arm64 不在本轮正式发布范围；后续支持须增加独立构建和验证。
- 保留现有 onedrive-cpp 包名、CLI 可执行文件名、配置路径和
  systemd 用户服务名，不因 GUI 合包迁移用户配置或改变服务入口。
- 两个发行版分别在对应环境构建和验证，不共用 DEB 二进制，
  不通过修改包名后缀伪装兼容性。
- 使用同一上游应用版本，Debian 包版本采用各自发行版后缀
  （例如 -1~ubuntu24.04 与 -1~ubuntu26.04）；Debian revision 随包变更递增。
- 后续门禁须覆盖已发布旧版 onedrive-cpp 的升级，检查配置保留、
  CLI 和服务兼容性，以及 Ubuntu 26.04 合包引入的 Qt 依赖。
