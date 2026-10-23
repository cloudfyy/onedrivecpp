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
  - 安装后补充修复 Qt portal 注册顺序：在 QApplication 创建前设置桌面应用标识，
    避免外观设置查询先占用 D-Bus 连接。真实 Wayland 会话的最小程序已复现
    旧顺序报错并验证新顺序注册成功；修复纳入 1.0.2-1 安装包，
    不禁用 portal 或过滤日志，完整桌面交互仍需复测。
  - 1.0.2 将同步受阻的退出码由 2 改为 3，命令行参数错误继续使用 2；
    双语说明和 man 手册同步更新，调用脚本须按新约定判断受阻结果。
- [x] P1 保护 CPack 系统配置升级
  - 为系统配置声明 conffiles，验证升级保留用户修改。
  - 核对 Debian 原生包的配置文件处理。
  - 已为 CLI 和 GUI CPack 包生成 conffiles 元数据，匹配实际配置安装路径，
    包括自定义绝对路径和相对路径配合自定义打包前缀。
  - 原先“升级覆盖修改”和“remove 删除修改后的配置”两项失败均已修复。
    remove 现在保留系统配置，purge 才删除。
  - 旧版未登记 conffile 的包迁移采用显式 --force-confold 策略，
    已受保护包之间的升级使用默认策略；两种隔离生命周期回归各 20 项通过。
  - 新增实际 DEB conffiles 内容和对应安装路径的 CTest 检查。
    使用原生 dh_installdeb 核对了 /etc 下配置的自动登记，无需修改 Debian 规则。
  - 回归脚本与结果保存在当前会话 files/conffiles-lifecycle/ 和
    files/package-lifecycle/，宿主机对应安装文件未改变。
- [x] P1 执行当前安装包的隔离生命周期检查
  - Ubuntu 26.04 上使用非特权 Bubblewrap、临时写时复制目录及独立 dpkg
    数据库执行真实安装、包修订版升级、remove 和 purge。
  - 初始 19 项检查中 17 项通过、2 项配置保留检查失败；
    两项失败现已在配置保护项中修复并完成回归。
  - 已验证 CLI-only 包升级到 GUI 合包、安装后程序启动参数、桌面入口
    offscreen 启动、GUI 文件和 Qt 依赖、用户服务离线校验及 enable/disable。
  - 升级测试使用相同上游应用版本的包修订版 -1 到测试修订版 -2，
    不代表更早应用版本的数据迁移验证。
  - 测试继承宿主机已安装依赖，不是干净发行版环境；
    dpkg 使用 --no-triggers，尚未验证触发器及桌面缓存更新。
  - 未覆盖真实 X11/Wayland 显示、systemd 实际启动和账户同步、
    Ubuntu 24.04 环境及 Debian 原生构建。宿主机对应安装文件未改变。
  - 可重跑脚本、结果 JSON 和日志保存在当前会话 files/package-lifecycle/。
- [x] P1 完善干净离线构建依赖
  - 约束 FTXUI 兼容版本，明确 FTXUI 和 Proxy 的来源及离线供应方式。
  - 完善 Debian 构建依赖和第三方版权声明。
  - 在干净目标环境验证构建不依赖宿主机缓存。
  - 采用方案 1：联网准备发布源码归档，固定依赖版本、来源和 SHA256 统一记录于
    packaging/dependencies.json；third_party 源码归档随发布源码提供，不提交到 Git。
  - tools/prepare_source.py 打包 Git 已跟踪文件和校验后的依赖；可复用缓存离线准备，
    归一化元数据后重复输出逐字节一致。
  - Debian 始终启用 ONEDRIVE_OFFLINE_BUILD，缺失或损坏归档明确失败；
    不选择已安装的 FTXUI/Proxy，也拒绝 FetchContent 源码目录覆盖。
  - 开发模式拒绝旧版 FTXUI 5；系统包兼容范围为 FTXUI 7.x（至少 7.0.3）
    和 Proxy 4.x（至少 4.1）。Proxy 改用带 LICENSE 的完整源码归档，
    原有 13 个头文件逐字节一致。
  - 补齐 Python/Git 构建依赖、第三方版权声明和二进制包许可证安装。
  - 已在 Ubuntu 26.04 的全新源码/构建目录中，通过 Bubblewrap 禁网并隐藏
    宿主机 HOME，完成 GUI 与 CLI 构建及 DEB 生成；相关 CTest 分别 10/10、
    8/8 通过，另有源码准备工具 8 项单测和 6 项依赖选择/校验检查通过。
  - 本次使用宿主机已准备好的系统库及独立提供的 Qt 工具；全新系统镜像、
    Ubuntu 24.04 和完整 Debian 原生打包仍留待发布门禁，不承诺二进制逐字节一致。
- [x] P2 统一安装前缀和服务路径
  - 根据安装前缀生成 systemd 用户服务的 ExecStart。
  - 验证 /usr、/usr/local 和自定义前缀。
  - 服务模板在安装阶段生成，匹配实际前缀和 CMAKE_INSTALL_BINDIR，
    支持 cmake --install --prefix 与 CPack 默认 /usr，路径不含 DESTDIR。
  - 保留现有服务名及 lib/systemd/user 入口，将生成的服务登记到安装清单；
    安装过程不向构建目录写入服务文件。
  - 已覆盖 GUI/CLI 安装、三个前缀、自定义相对和绝对 bin 目录、
    DEB 内服务与可执行文件路径，并检查带空格、% 和 $ 的路径。
  - 使用 systemd-analyze 实际解析服务，提供独立运行目录，不依赖登录会话；
    禁网并隐藏宿主机 HOME 和 /run 的隔离环境回归通过。
  - 中英文说明补充自定义前缀下的用户服务链接方法。
    本项不启动实际同步服务，在线账户同步仍属于发布门禁。
- [x] P2 校验应用和发行版包版本
  - 检查 vcpkg.json、Debian changelog、CPack 版本及目标发行版的一致性。
  - 支持合法 Debian revision，不一致时显式失败。
  - CMake 在依赖准备前核对应用版本与 changelog 上游版本；两份元数据变动会
    触发重新配置。CPack 打包前再次核对源码与已配置版本，拒绝过期默认修订号
    和不一致的版本覆盖，不能靠覆盖包版本绕过过期配置。
  - 修订号默认跟随 changelog，支持 ONEDRIVE_PACKAGE_REVISION 显式覆盖及
    Debian 合法的字母、数字、点、加号和波浪号，不再固定为 1。
  - Debian 规则启用 ONEDRIVE_DEBIAN_PACKAGE_BUILD，要求完整包版本与
    实际构建发行版匹配，suite 与 VERSION_CODENAME 一致。
    普通开发和快速 CPack 仍允许 changelog 面向另一发行版。
  - 应用版本保持 1.0.1；本轮打包改进的修订号升至 2，仓库 changelog
    继续以 Ubuntu 24.04/noble 为基准，26.04 构建须更新对应条目。
  - 新增独立版本矩阵回归及真实 DEB Version 字段断言，覆盖修订号覆盖、
    合法复杂修订号、旧配置、错误版本、发行版和 suite 不匹配。
    已生成 1.0.1-2~ubuntu26.04 的 GUI 合包，并验证修订号 3 的 CLI 包。
  - 完整 dpkg-buildpackage 和 Ubuntu 24.04 环境验证仍留待发布门禁。
- [x] P2 修正发行包元数据
  - 修正项目来源、维护者占位信息和过时描述。
  - 为 CPack 包补齐许可证及版权文件。
  - 将版权来源和 Homepage/VCS 指向 cloudfyy/onedrivecpp，
    当前维护者采用已有 changelog 中的实际联系信息，不改写历史签名。
  - 移除 scaffold 描述，说明现有同步、CLI/TUI、用户服务及可选 Qt GUI。
    CPack 从 debian/control 读取维护者、主页和完整描述，避免两套元数据漂移。
  - LICENSE 补入 GPLv3 完整文本，保留 GPL-3.0-or-later 声明；
    CMake 安装和 DEB 均包含 LICENSE 与机器可读 copyright。
  - 在现有真实 DEB 回归中增加独立元数据解析和许可证文件逐字节断言，
    GUI/CLI 两种包均通过；dpkg-gencontrol 也已核对原生控制文件生成。
  - 同步中英文项目文档；未执行完整原生打包或 Lintian，
    这些检查仍属于发布门禁。
- [ ] P2 建立隔离构建与发布门禁
  - 以 Debian 原生打包为正式主线，CPack 为快速试用入口。
  - 按目标平台建立容器或 chroot 干净构建。
  - 加入编译测试、包内容断言、版本检查、Lintian、安装启动、
    配置保留升级及卸载检查。
  - 验证后发布产物与校验和。
  - 打包修订号升至 1.0.3-2，软件版本保持 1.0.3，不移动已有 v1.0.3 标签；
    后续打包发布使用 v1.0.3-2。
  - 已用验签并校验 SHA256 的官方 Ubuntu 24.04 base rootfs，独立安装发行版
    构建依赖，以非 root 身份禁网完成 dpkg-buildpackage，121 项测试全部通过；
    原生 DEB 和调试包已生成，Lintian 无错误。
  - 启用真实 man 后，Lintian 的 groff 检查报告渲染子进程崩溃警告；
    同环境直接渲染包内中英文手册成功。首次 Actions 的 Ubuntu 24.04 构建
    和生命周期检查已通过，日志未出现该 groff 警告；本地警告仍保留在报告中。
  - 已在该隔离系统中验证本地安装、模拟旧包升级、修改配置保留、CLI 启动参数、
    中英文压缩 man 手册、服务语法及离线 enable/disable、remove 和 purge；
    man-db 触发器正常执行，无 Qt 运行依赖，未改变宿主机安装。
  - 新增双平台 GitHub Actions、构建镜像和可复跑的生命周期脚本：
    PR/main/手动运行执行门禁，匹配版本的标签在两平台通过后生成 Release 草稿，
    附带准备好的源码、二进制包、构建元数据和 SHA256SUMS，正式发布仍需人工审核。
  - 最小镜像恢复真实 man 程序及手册安装，测试明确核对中文路径，避免占位程序
    或英文回退掩盖缺失；本地包由 APT 安装，网络由容器隔离层禁用。
  - 首次 Actions（39884659346）中 Ubuntu 24.04 原生构建和生命周期检查通过；
    Ubuntu 26.04 在 Clang 链接 console_tests 时出现未解析的模板和内联符号。
    切换 GCC 的 PR #1 又在数据库迁移测试中编译失败，不采用该编译器切换方案。
  - 已在 Ubuntu 26.04 禁网隔离环境用相同源码对照确认：启用发行版的
    LTO／fat-LTO 可复现链接失败，关闭后 console_tests 和 db_migrate_tests
    均可构建。Debian 规则改为 hardening=+all optimize=-lto，继续使用
    Clang 20，保留 Release 优化及安全加固。
  - 本机 Docker 服务不可用，Ubuntu 26.04 对照使用宿主系统库和独立源码／
    构建目录，不等同全新发行版镜像。修复后的双平台原生 Actions 仍待重新运行，
    本项暂不勾选完成。
  - 关闭 LTO 后，Ubuntu 26.04 在同一禁网隔离环境完成全量 GUI 构建，
    140 项测试通过，并生成 1.0.3-2~ubuntu26.04 的 CPack GUI 合包；
    Ubuntu 24.04／26.04 均已核对 Debian 子进程不再继承 LTO 参数，
    同时保留安全加固。此结果不替代修复后双平台 dpkg-buildpackage 门禁。
  - 第二次 main Actions（39903100980）中 Ubuntu 24.04 全流程通过；
    Ubuntu 26.04 编译及链接通过，但三个测试的 read/poll 拦截遗漏 Fortify
    生成的 __read_chk／__poll_chk，导致故障注入失效。此前本地 CMake 验证
    漏带 CPPFLAGS 中的 _FORTIFY_SOURCE=3，现已补充实际加固调用路径复验。
  - 测试为普通及加固符号复用注入逻辑，未拦截的加固调用仍传递完整缓冲区长度
    给真实检查函数，不关闭生产加固或跳过测试。三个测试在非加固路径通过，
    加固路径各连续三次通过；修复后的云端全流程仍待运行。
  - 模拟旧包不替代所有历史发布包升级验证；真实桌面交互、systemd 登录会话启动
    和在线账号同步仍须发布前单独检查。未创建新远端标签或 Release。
- [x] P2 增加编译与依赖镜像缓存
  - 工作流使用 Buildx 按目标发行版和架构缓存依赖镜像层，每次检查更新的基础镜像。
  - 容器安装 ccache，以 CMake compiler launcher 接入，按发行版、架构和
    构建配置隔离快照，通过 Actions 缓存跨提交复用，每个快照上限 1 GB。
  - 源码固定解包到 /work/source；编译器按内容校验，不设置 sloppiness。
    仅 ccache 原生构建关闭 PCH，避免放宽预编译头的宏／时间检查；
    未启用缓存时保留原有 PCH。
  - 仍然每次执行配置、链接、测试、打包、Lintian 和生命周期门禁，构建及安装
    容器继续禁网，不复用旧安装包。ccache.txt 随日志上传，失败时也保留统计。
  - Ubuntu 26.04 禁网环境对 console_tests、db_migrate_tests 和 GUI 做冷／
    热重建：500 秒降至 6 秒，194 次编译全部命中，零 miss、零 PCH 拒绝；
    同一输入的三个可执行文件逐字节一致。该结果不代表完整 Actions 耗时。
  - actionlint 与内嵌 Shell 语法检查通过；本机 Docker 服务不可用，真实
    Actions 的缓存上传／恢复及 Buildx 镜像层命中仍待推送后验证。
- [x] P2 更新中英文安装包制作文档
  - 说明构建入口、目标环境、依赖准备、版本规则和配置升级行为。
  - 区分正式 Debian 包与 CPack 试用包。
  - 已同步修订号、双平台工作流、草稿发布、标签规则、权限、制品和校验和，
    并说明一次性容器的生命周期检查及未覆盖的真实使用场景。

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
