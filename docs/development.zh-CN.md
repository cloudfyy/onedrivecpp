# 开发与测试

[English](development.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## GUI 与 CLI 功能同步

新增或修改面向用户的业务能力时，必须同步更新对应的 GUI 与 CLI 入口，
并同时维护帮助、文档和回归测试。复用共享的应用查询／服务与策略规则，
不要在两个前端分别实现业务逻辑。纯展示控件不要求界面形式一致；
有意保留的能力差异必须明确记录，不能默默延后。

阻止项查询共用 `load_blocked_items` 离线访问状态，以及 `matches_blocked_item`
进行原因精确匹配和可搜索字段选择。前端只提供文本匹配方式，保留 GUI 的 Qt Unicode
匹配和 CLI 已说明的 ASCII 匹配，不重复维护字段筛选或数据库访问逻辑。
CLI 输出继续复用现有 Console 事件及文本／JSON／TUI 后端。

## 共享传输进度

上传与下载复用 `TransferProgressEstimator` 和既有的进度节流器；
`DownloadProgressEstimator` 保留为兼容别名。累计完成字节与本次操作新增网络载荷
字节分别传入；历史恢复偏移和复用文件可以推进完成进度，但不得产生虚假的速度或
剩余时间。上传重试计入实际发送字节，重复回调不能重复计数。

`SyncContextEvent`、`SyncPhaseEvent`、`SyncSummaryEvent` 分别传递前端无关的方向、
阶段和本轮数量，操作生命周期事件保持独立。上传批次完成不等于整轮同步完成，
Monitor 每轮结束也不等于监控退出；GUI 最终结果仍以清理完成后的 worker 结束为准。
新增事件必须同时接入文本、JSON、TUI、GUI 转发和 GUI 历史消费者。

## 跨进程同步占用

前端须在整个操作期间保留 `RuntimePreflight`，持有状态目录 OS 锁并校验认证和路径，
但不提前创建、探测写入或修改同步目录权限。解析实际账号／Drive 根目录后，共享
同步和 Monitor 服务先取得目录占用锁，再打开 item store 或准备数据目录；单文件下载
及清理复用同一取得锁的入口。Monitor 包括等待期间都持锁，内部轮次不重复获取。

每个有效 UID 使用私有本地登记目录。短期 `flock` 登记锁将记录检查、陈旧记录回收和
新登记串行化，每个活跃记录单独持有 OS 锁，保存规范化路径、根目录／祖先的设备和
inode 身份，以及限长、带版本的持有者元数据。路径或文件系统身份重叠均拒绝；
只有 OS 锁已释放的记录才能删除，活跃记录损坏时明确失败。RAII 和 `O_CLOEXEC`
保证清理、崩溃和 exec 的释放语义，不依赖 PID 存活判断。

`RuntimeBusyError` 保存结构化占用信息，前端通过共享 `runtime_busy` section／message
展示，不解析异常文本。回归使用 exec 后的真实子进程覆盖不同操作配对、相同／
重叠／缺失／别名目录、同时启动竞争、崩溃释放及 Monitor 等待时的共享服务占用。

目录规范化复用 `canonical_directory`，统一处理缺失根目录末尾分隔符等别名。
目录校验返回已规范化的同步路径，取得占用锁时不再重复解析。应用层目录准备复用
原有 `prepare_sync_root` 的创建和权限处理，写入探测保持独立，引擎自身的边界检查
仍保留。锁文件通过 `open_path_no_symlinks` 打开，锁检查与 `SafeSyncRoot` 共用
工具层 `FilesystemIdentity` 表示。

锁元数据复用 `read_private_file` 的描述符入口，借用已加锁的描述符，通过 `pread`
读取，不改变偏移、不重新打开路径；路径入口也复用同一套校验和读取代码。
元数据写入继续原地执行，不能用原子替换文件的写入函数，否则会更换 inode，
使路径脱离原有 OS 锁。

## 共享 Monitor 控制

`monitor::request_control` 和 `ControlServer` 是前端无关的本地 IPC，供 CLI 与后续
GUI 控制共用。生产 Monitor 在 `run_loop` 内持有 server，外层状态／根目录锁始终
有效。仅处理 IPC 的线程在同步回调阻塞期间仍可响应，不执行第二套同步引擎。
带版本、限长的 Unix seqpacket 消息拒绝任意配置／目录字段；两端通过 `SO_PEERCRED`
校验身份，私有目录和 socket 检查复用文件系统安全工具。清理前核对端点身份，
不删除被替换的文件。

手动待处理请求通过 mutex 和 eventfd 传给原调度器，组合取消 token 同时响应外部
与 IPC 停止。手动／本地／远端待处理变化复用既有状态机，合并为串行补查。
首次同步前监控根目录；根目录尚未创建时先监控最近已有祖先。重建 watch 前先处理
队列。同批事件内创建又删除的文件没有剩余本地工作，避免能力探测文件导致
无限重触发，不按用户文件名过滤。停止确认先于回调清理和锁释放，后续触发被拒绝；
失败／阻止结果可查询，沿用既有继续监控策略。

exec 后的真实子进程回归覆盖状态响应、请求合并、执行中新变化、取消、清理期间
持锁、CLI 离线文本／JSON、崩溃重启、非法消息／响应、权限／端点替换及超时。
本步骤有意暂缓 GUI 监控按钮，后续复用该客户端／调度器，不隐式转发普通同步。

## GUI 本地化

启用 `ONEDRIVE_BUILD_GUI=ON` 时需要 Qt 6.8+ 和 Linguist 工具；
`qt6-translations-l10n` 提供标准按钮和对话框使用的 `qtbase_zh_CN.qm`。
只有启用 GUI 时，缺少这些工具或翻译包才会导致配置失败。自定义 Qt 安装可以
通过 CMake 缓存项 `ONEDRIVE_QTBASE_ZH_CN` 指向对应版本的翻译包。

GUI 文本使用 `tr()` 或 `QCoreApplication::translate()`；账号与配置视图模型
使用枚举而非已翻译字符串，其无界面测试仍不依赖 Qt。`Localization` 在构建
控件前安装资源内嵌的翻译器，并保持到应用退出。简体中文系统语言使用 `zh_CN`
翻译包；其他语言回退到英语源文本。后端原始错误详情保留，不进行翻译。
GUI 专用的 `QCommandLineParser` 在安装翻译器、创建窗口前解析
`--language auto|en|zh_CN`；`--help`／`--version` 提前退出，非法值或未知参数
明确报错。启动时不弹出语言选择框。运行时语言菜单调用应用持有的
`Localization` 控制器，通过 `LanguageChange` 更新现有控件和保留的登录／同步消息，
不重建窗口、不重置表单、不干预活动线程。语言选择仅本次运行有效，
CLI/TUI 参数解析器保持不变。
本地化回归测试还覆盖浏览器授权期间以及账号刷新成功／失败后的语言切换。
Qt URL 处理器捕获有效浏览器请求，不启动真实浏览器；格式错误、非 HTTPS、
缺少主机及含用户凭据的 URL 不得到达处理器。通过显式控制工作线程完成时机，
避免错误和状态断言依赖线程调度。启动测试验证 `auto` 与显式覆盖即使解析到
相同语言环境也保持不同选择，并拒绝与帮助参数同时出现的位置参数。

修改 GUI 文本后，使用 Qt 工具提取字符串，在
`src/ui/gui/translations/onedrive_zh_CN.ts` 中翻译所有新增条目，再重新构建：

```bash
cmake --preset debug -DONEDRIVE_BUILD_GUI=ON -DONEDRIVE_ENABLE_GRAPH_E2E=OFF
cmake --build build/debug --target onedrive_gui_lupdate
cmake --build build/debug --target onedrive-cpp-gui gui_settings_tests gui_startup_state_tests gui_localization_tests gui_startup_options_tests gui_login_tests gui_synchronization_tests gui_state_view_model_tests gui_system_appearance_tests browser_request_tests
ctest --test-dir build/debug -R '^(gui_.*|browser_request_tests)$' --output-on-failure
```

`lrelease -nounfinished` 将翻译编译成 `.qm`；Qt 资源将应用和 Qt 翻译包内嵌到
GUI 及测试程序。不要手动编辑生成的 `.qm` 文件。离屏测试分别在英语、
`zh_CN`、`zh_SG`、法语和繁体中文环境运行，验证资源加载、英语回退、翻译器
生命周期、真实窗口标签、账号／配置枚举、登录结果、设置对话框和 Qt 按钮、
占位符替换及 UTF-8 loopback 回复。启动参数测试在中英文环境使用真实 Qt
解析器和翻译器，覆盖语言覆盖、`--language value` 与 `--language=value`。
离屏可执行文件子进程验证帮助、版本及非法参数不会进入 GUI 事件循环；
菜单切换测试验证勾选项、已有控件和状态翻译、未保存字段及活动登录的保留。
无需 Graph 凭据或真实浏览器，但不能替代
真实桌面及原生对话框验证。

设置改版第一、二阶段已实现账号与目录、同步规则、传输、网络、监控、高级与日志，
包含高级账号端点、权限范围数组及挂载要求。GUI 监控任务、目录迁移与服务
集成仍暂缓。`SettingsEditor` 六个子页共用基于控件的草稿；枚举
`itemData` 保存解析器值，不从翻译文本反推。数字文本框避免 Qt 32 位整数
截断，验证 TOML 非负 int64 范围。只修改已编辑键；选择性规则留空可删除可选键，
更改同步模式时移除旧 `sync.upload`，同时保留选择的删除策略。核心补丁工具使用
解析器源位置处理引号表头、点分键与内联表，不用正则或整文档序列化。
数组及嵌套 Graph 限流表保留源码结构。代理 URL／绕过列表覆盖分别保持未设置与
显式空值，未编辑的空代理表也保留；移除所有代理设置可移除表而保留周围注释。
GUI 保存／导入拒绝 URL 内联凭据及不安全的认证／Graph 端点；密码／CA 文件只作
引用，运行时安全检查不变。保存先验证，再备份与原子替换。导入／重新加载重置所有草稿；语言切换和登录
完成不会丢弃草稿。

`gui_settings_tests` 使用真实离屏控件与保存后的 TOML 验证所有新增字段、64 位
边界、解析器约束、注释／无关配置保留、备份、可选键删除、草稿／语言／子页
切换、重新加载及首次应用 ID 设置。提示测试验证全部 57 项简短说明及控件、标签、
浏览按钮、覆盖行的无障碍描述，初始中英文与不支持语言的英文回退、重新加载后的
禁用继承控件，以及保持高级分组展开和未启用代理草稿的运行时语言切换。
可用 `ctest --preset debug -R '^gui_settings_tests$'` 单独运行。
另覆盖所有传输排序／同步模式的重新加载、
数值上下界与严格草稿语法、高级分组展开／收起、浏览选择／取消、内联表各位置的
可选键删除、UTF-8／多行源码编辑、非法补丁不写入／不备份、保存／导入取消、
非法导入及首次导入按目标配置位置解析规则文件。未知 TOML 键会被拒绝并保留原文，
不会静默丢弃。启动／导入回归验证损坏配置恢复；本地化与
登录测试覆盖禁用控件及工作线程／草稿保留。不代表已提供目录迁移或
在线凭据验证。第二阶段另覆盖所有新增字段、代理模式／可选状态、嵌套／内联／
点分表与数组内部注释、超时单位、退避／限流组合约束、端点安全及错误不写入／不备份。
覆盖率补充测试验证 GUI 空覆盖值／空 scopes、独立继承的代理凭据、NUL／空文件引用
拒绝、安全表删除、空内联限流父表及 int64 超时／限流边界。`proxy_basic_tests`
通过回环 Digest 挑战验证环境代理认证，并验证空代理覆盖值使用直连。

`gui_synchronization_tests` 使用离线 Graph／存储适配器执行真实应用同步服务，
验证预览保留同步文件、实际同步下载正确字节及持久化 dry-run。控制器测试覆盖
工作线程／GUI 线程边界、运行锁、取消、重启与析构、日志恢复、500 事件队列及
进度合并。窗口测试覆盖双语进度／历史、阻塞及失败结果、账号／设置操作互斥、
未保存草稿、有明确省略提示的历史上限，以及关闭时取消并回收线程。
Graph／应用服务测试还验证令牌刷新和 Drive 身份查询可在创建存储前取消。
`sync_preview_tests` 覆盖合并／替换模式的预览游标复用、重复预览、同 ID 最新变更合并、
尚未应用的变更、游标过期和目录层级回退，以及账号／配置范围、筛选、状态和恢复记录
失效、本地冲突重查、远端删除保护、取消及失败。GUI 控制器集成使用真实 SQLite
ItemDatabase 验证预览不持久化状态，增量同步提交最新游标并下载正确字节，以及已保存
配置／账号／外部状态变化和显式失效禁止复用。任务运行期间缓存只由 worker 访问，
账号操作只在空闲时清除；配置指纹使用安全读取后传给解析器的同一份字节。
Graph 分页去重与预览缓存合并共用私有 `graph/delta_changes.hpp` 累加器，保持首次出现
顺序，同 ID 的最新完整记录替换旧记录，包括删除、恶意文件、校验及哈希字段。
协议测试补充传输失败、非法 delta 页以及请求前／分页间取消。缓存回归还覆盖跟踪目录
回退、完整快照派生删除、集合顺序变化、持久化哈希变化、空游标、完成边界取消、
完整查询回退失败，以及选择性保留的本地文件／上传抑制；本地路径检查失败时清除缓存，
不改变用户内容或已应用状态。应用测试在提前取消、身份
失败和启动观察者异常前建立真实缓存；GUI 测试使用空白凭据覆盖默认生产执行函数，
在 HTTP 请求前本地失败，并验证 worker 运行期间禁止清除缓存。
不支持格式只由编译进程序的私有 `src/graph/unsupported_formats.hpp` 规则表配置。
每条规则使用稳定、非空且唯一的标识及显示名称，MIME／后缀需同时命中，package
类型独立匹配。`RemoteItem.unsupported_format` 和待应用 Delta 行保存格式标识；
持久阻塞记录使用 `unsupported_<id>` 原因码。共享元数据和包后代复用通用分类。
整个规则表参与同步筛选和待应用缓存指纹；新增格式会刷新元数据，无需修改应用
流程或为每种格式增加数据库迁移。

`sync_unsupported_tests` 使用真实 SQLite 验证重启／重试、规则策略刷新、正常文件
进度计数及严格哈希失败、包后代、双向上传排除，以及远端删除／全量刷新时保留已有
笔记和快照。Graph 辅助／协议测试覆盖所有内置规则、后续页父包、共享项目、普通
`.one` 文件和元数据错误。单文件及 GUI 双语回归覆盖明确的不支持结果、数量、
翻译历史和严重级别筛选。

页面／日志回归验证概览、同步、日志、设置四页的布局与导航、概览摘要、
跨任务历史及精确省略计数、时间戳、全部级别筛选、复制显示内容、同步中清空、
自动滚动开关、原始多行详情，以及双语切换保留历史／筛选／草稿。
账号测试验证浏览器授权参数不会进入本次运行日志。
日志回归保留前后端共用的六个级别，验证 GUI 全部配置阈值（含 `off`）、失败结果前
送达诊断、恢复原日志器及重启。TUI 测试验证低级别后端日志抑制、全部应用消息级别及
非法级别拒绝；伪终端使用离线认证适配器运行真实 CLI 应用，验证后端日志转发至 TUI，
同时已启用的全部级别仍写入私有日志文件。空数据库健康检查及关闭窗口时完成登录的
结果使用信息级别，不再单列成功级别。
显示阈值回归还验证同步中的新日志、空筛选结果、复制、语言切换以及清空历史后
保留低级别新消息。像素检查验证明暗调色板下 0%、1%、50%、100% 的蓝色进度，
并检查未填充区域。CLI 应用测试遍历全部日志阈值（含 `off`），验证命令结果不受
后端诊断阈值影响。
进度回归明确区分未知扫描／下载总量、扫描完成和 Delta 汇总、文件数／字节百分比，
以及下载完成与整个任务完成。`EndDownloadProgressEvent` 只结束终端进度行，
不得重置 GUI 下载进度。测试还验证晚到的扫描汇总不覆盖取消状态，下载完成／停止后
不再显示剩余时间估算。
传输数量按界面语言添加千位分隔，数据量／速度使用 IEC 单位，剩余时间按秒／分钟／
小时／天紧凑显示。GUI 传输、CLI 进度与容量／检查信息共用不依赖 Qt 的
`util/byte_size.hpp` 换算工具，各自保留数字精度、区域格式与既有最大单位。
回归覆盖单位与时间边界、零值及 uint64 大数、界面语言与默认区域
设置不一致，以及任务运行期间切换语言。
`sync_persistent_delta_tests` 重新创建真实 SQLite 存储和同步引擎，覆盖完整／合并模式
中断后恢复、最新记录／删除、空增量、已完成文件复用及字节偏移续传。共享引擎在规划／
执行前保存未筛选、未增补的远端元数据，不推进已应用游标。预览可以读取兼容批次，
但不修改它；上下文／筛选／已应用基线不匹配时失效，游标过期和目录结构变化回退。
`db_pending_delta_tests` 覆盖原始字段与顺序、替换／提交清理的事务原子性、只读访问、
重置／清理及 schema 26→27、27→28 升级。批次头与变更行分别存储，避免整个批次受到 SQLite
单值大小限制。文件逐个提交与恢复会改变跟踪状态，因此持久化复用不要求状态集合
完全相同，与会话预览缓存的校验不同。
CLI 应用与 GUI 控制器重建回归也使用真实 SQLite，验证新的入口从已获取游标进行
增量查询、执行保留的变更，并原子提交与清除待处理批次。

## 离线代码覆盖率

启用 `ONEDRIVE_ENABLE_COVERAGE=ON` 可使用 Clang 的源码覆盖率插桩，
同时要求 `BUILD_TESTING=ON`。正常构建默认关闭此选项。在 VS Code CMake Tools
中选择 `coverage` 配置、构建和测试 preset，即可使用独立的 `build/coverage`
目录，默认禁用真实 Graph 测试。Qt 为可选依赖；有 Qt 时可在此配置中开启
`ONEDRIVE_BUILD_GUI=ON`，纳入 GUI 代码和测试。

CTest 在 `build/coverage/coverage/profiles` 下生成分进程 profile。
每次全新测量应使用空的 profile 目录，不要混用不同源码版本的结果。
运行完测试后，用以下命令生成合并数据、详细 JSON 和按文件统计的文本报告：

```bash
python3 tools/coverage_report.py build/coverage
```

默认要求 `llvm-cov-20`、`llvm-profdata-20` 和 `readelf`，可通过命令行参数指定
与编译器匹配的 LLVM 版本。报告按 Build ID 匹配每个程序的 profile，
避免不同程序的同名 `main` 执行计数串用；生成每个程序的 LCOV、按文件统计的
JSON（含未覆盖行号）及文本摘要。报告排除测试、依赖与生成代码；没有执行的
已构建 GUI 与程序入口计入未覆盖范围。工具不访问外部 debuginfod，
并拒绝混入旧 Build ID。LCOV 按源码位置合并行和分支，与 LLVM 按模板实例统计
的百分比口径不同。不能以测试通过代替覆盖率或真实桌面行为验证。

离线故障回归测试使用测试程序专属的链接器 `--wrap`，不修改生产代码。
原子写入测试验证重试、rename 提交边界和清理错误；元数据测试模拟 xattr
探测、降级和写入错误，不依赖宿主文件系统支持 xattr。HTTP 测试仅使用
loopback 响应，覆盖 checkpoint 异常、close 失败和回滚错误优先级；
移动恢复测试通过模拟存储在持久化边界制造冲突，验证安全恢复。

```bash
cmake --build build/debug --target atomic_file_failure_tests filesystem_metadata_failure_tests http_download_failure_tests sync_move_recovery_tests
ctest --test-dir build/debug -R '^(atomic_file_failure|filesystem_metadata_failure|http_download_failure|sync_move_recovery)_tests$' --output-on-failure
```

这些测试无需 Graph 凭据、浏览器或资源耗尽。

授权与线协议回归还在编译期检查 PKCE 的全部 64 个状态转换组合，
验证仅可移动的秘密清理（包括短字符串和异常展开），并用确定性的事件测试
浏览器/Socket.IO 归约器。`auth_code_failure_tests` 通过测试程序专属的
URL 解析器/随机数包装验证异常传播、终态失败和部分随机输出清理。
GUI 集成测试只使用本地 loopback 连接。
`monitor_socket_transport_tests` 仅在测试程序中包装 libcurl 和 poll，
验证生产适配器确实执行归约器命令，并跨 `CURLE_AGAIN` 保留分片文本；
无需网络或真实浏览器。

```bash
cmake --build build/debug --target auth_code_tests auth_code_typestate_tests auth_code_failure_tests browser_request_tests gui_login_tests monitor_socket_tests monitor_socket_transport_tests monitor_notify_tests
ctest --test-dir build/debug -R '^(auth_code_tests|auth_code_typestate_tests|auth_code_failure_tests|browser_request_tests|gui_login_tests|monitor_socket_tests|monitor_socket_transport_tests|monitor_notify_tests)$' --output-on-failure
```

阻止项回归中，`app_queries_tests` 验证 SQLite 只读查询、配置／实际 Drive ID
选择、损坏或有歧义的元数据及不安全的状态路径。`graph_support_tests`、
`sync_unsupported_tests` 和 `blocked_policy_tests` 覆盖祖先验证／取消、
同一 Drive 的上下文筛选，以及文件精确匹配与目录子树恢复保护。
`gui_synchronization_tests` 检查中英文策略说明；`download_single_tests`
验证非法远端元数据和本地路径冲突落库，且不传输内容、不修改同步根目录外的文件。
这些场景无需在线访问 Graph。

重建覆盖率程序后，应归档原有的 `coverage/profiles` 目录，再新建空目录并运行
CTest。用 `python3 tools/coverage_report.py build/coverage --name state-machines`
生成具名报告；同时检查私有归约器头文件和适配器源文件。LLVM 模板分派/访问器的
`no-profile-record` 警告（包括已知 hash 为零的 Proxy 访问器）与未覆盖源码分支
不是同一口径，需单独说明，不能据此宣称每个模板实例均完全覆盖。

测量结束后切换回 `debug` 或 `release` preset 即可恢复正常开发；
它们的独立构建目录不受覆盖率 preset 影响。

## 真实 Graph 端到端测试

普通构建不会启用 `e2e` preset。这项测试需要专用测试账号或 Drive，并要求远端
根目录预先放置内容稳定的测试夹具（fixture）。仓库外的配置必须已经完成认证，
且绝不能提交到仓库。测试运行器（runner）会把状态目录复制到隔离的临时工作区，
只清理这份副本，并配置独立的 `sync_list`。
它会验证以下行为：显式单文件下载可以绕过空规则文件；之后的同步会排除该文件的
快照，但不会删除本地文件；加入包含规则后，会触发完整远端状态查询并重新下载
fixture；最后一次增量同步不会替换未变化的文件。随后，runner 会写入发生冲突的
本地内容，并只清理隔离的状态副本。测试会确认
`sync.local_conflict = "backup"` 能发出结构化事件，将本地字节完整保存为同一
目录下唯一的 `safeBackup`，恢复 Graph 上的权威 fixture，并持久化一条快照；
后续增量同步也不会改写远端文件或备份。runner 会强制隔离配置使用 `backup`
策略，但不会修改外部配置。它还会创建可丢弃的本地子树，用来验证远端目录创建、
简单上传、超过 250 MB 的 upload session、已完成上传的恢复、本地与远端同时
修改时的冲突备份、移动到新父目录，以及本地删除。大文件默认为 250,000,001
字节；
`ONEDRIVE_E2E_LARGE_UPLOAD_BYTES` 可以把它增大到 1 GB，但不能降到 upload
session 阈值以下。随后，它会使用复制账号中的 token 创建另一个名称唯一、可丢弃的 Graph 子树，
再将一个文件和一个目录移动到其他远端父目录并重命名。测试会验证同步过程复用了
相同的本地 inode、更新了快照，并发出结构化移动事件。即使测试失败，远端临时
子树也会被删除。另一个可丢弃子树覆盖七个补充互操作场景：双向 dry-run 不改变
本地、Graph 或快照；upload-only 与 download-only 的方向隔离；零字节文件；
包含空格和 Unicode 的名称；无冲突的远端内容替换；以及单个远端文件删除。

最后，runner 会向隔离状态注入一条可信快照，其 ID 不存在于真实的完整 Graph
Delta 响应中。测试将验证本地安全删除、SQLite 清理和结构化输出，同时确认真实
fixture 保持不变。它还会启用 `sync_root_files`，验证选择摘要的变化会触发完整
Graph 查询，并确认已被规则选中的 fixture 不会重写。

### 配置与运行

```bash
export ONEDRIVE_E2E_CONFIG=/absolute/path/to/dedicated-e2e.toml
export ONEDRIVE_E2E_EXPECTED_PATH=fixture/small.bin
export ONEDRIVE_E2E_EXPECTED_SHA256=<64位小写十六进制值>
# 可选；必须大于 250,000,000，且不能超过 1,000,000,000。
export ONEDRIVE_E2E_LARGE_UPLOAD_BYTES=250000001

cmake --preset e2e
cmake --build --preset e2e
ctest --preset e2e -R graph_sync_e2e
```

runner 会输出阶段编号和百分比进度。CTest 默认不显示通过中测试的输出；只有
需要实时观察进度时才增加 `--verbose`。

稳定入口 `graph_sync_e2e.py` 会委托给 `tests/e2e/` 中的 Python模块；真实
Graph 场景、系统边界场景、进程编排、状态 fixture 和 runner 自测分别维护。
`live.py` 只保留基线流程，上传、远端移动和补充覆盖场景分别委托给职责单一的
`live_*` 模块；配置与 Graph helper 自测也与主自测 driver 分离。

### 测试数据与产物安全

专用 Drive 必须授予文件写权限，并且只能包含可丢弃的测试数据；runner 生成的
`sync_list` 只会落地预期 fixture 和临时 fixture。应预留足够的本地空间同时保存
大文件源和稳定上传快照，并确保测试账号有足够配额保存远端副本。设置
`ONEDRIVE_E2E_ARTIFACT_DIR` 后，失败时会保留命令输出和客户端日志；
这些诊断信息可能包含远端文件元数据，应按敏感数据保管。临时配置、复制的 token、
SQLite 状态和下载内容始终会被删除。不要让 E2E runner 使用日常状态目录或日常
Drive。

### 系统边界场景

live runner 还会使用真实可执行文件验证系统边界：本地代理主动断开连接及恢复、
确定性的本地上传存储耗尽及恢复、不可读上传源，以及可续传上传期间的
`SIGKILL`。Monitor 边界覆盖创建、修改、删除、文件及目录重命名、跨目录移动、
atomic save、burst 合并、create-then-delete、递归 watch、大文件 upload session，
以及真实 inotify 队列溢出后的恢复和正常 `SIGTERM` 退出。测试必须以非 root
用户运行，因为权限场景依赖普通 Unix 访问检查。CMake 还要求提供 `stdbuf`
命令，以便在不改变生产输出缓冲行为的情况下观察 Monitor JSON 事件。

每个边界场景都是独立的 CTest 条目，可通过以下命令运行全部五项：

```bash
ctest --test-dir build/e2e --output-on-failure -L boundary
```

## C++ Core Guidelines 检查

`lint` preset 会在编译时运行较快的 Clang-Tidy 策略。bug-prone、performance、
portability 以及选定的 C++ Core Guidelines 诊断都会作为构建错误：

```bash
cmake --preset lint
cmake --build --preset lint
```

快速 preset 有意排除了路径敏感的 Clang 静态分析器，因为它在大量使用模板的
翻译单元中可能非常耗时。定期检查或 CI 可使用深度检查 preset：

```bash
cmake --preset lint-deep
cmake --build --preset lint-deep
```

针对性复查应保持相同的 GUI 设置和深度分析策略，只构建受影响的对象目标，例如：

```bash
cmake --preset lint-deep -DONEDRIVE_BUILD_GUI=ON
timeout 15m cmake --build --preset lint-deep --target \
  CMakeFiles/onedrive_core.dir/src/graph/discovery.cpp.o \
  CMakeFiles/onedrive_core.dir/src/monitor/socket.cpp.o
```

对象复查不能证明未构建的翻译单元或超时扫描通过。第三方诊断应单独报告，
不能修改系统头文件或关闭检查来隐藏它们。

策略只排除经过审查的必要 C/POSIX API、协议常量和已检查缓冲区边界噪声。
项目使用 Microsoft GSL 在 API 和 RAII 边界表达非空借用依赖，并使用
ngcpp/proxy 提供具有明确拥有/借用适配方式的类型擦除运行时端口。

协议标识和日志级别应优先复用不依赖 locale 的 ASCII 比较，而非复制字符串后
转小写。同步借用文本使用 `std::string_view`，剩余 I/O 缓冲区使用 `std::span`
子视图。POSIX 调用继续由 RAII 对象管理；新语法不能削弱关闭错误检查、持久化
和错误报告行为。

分页和下载尝试 helper 拥有 callable，并在各页和重试之间以左值调用；
需要借用可变 callable 状态时使用 `std::ref`。Socket.IO 工作线程同时捕获
传输操作和失败报告中的异常。即使日志 sink 或错误处理器抛出异常，也分别
尝试记录日志和发布断连事件；分配失败时事件发布仅尽力而为，不能导致进程终止。

测试通过 `tests/support/common.hpp` 复用带错误检查的二进制文件 I/O 和异常匹配。
`throws_with<Exception>(operation, message)` 按消息子串匹配；未抛异常时返回 false，
指定类型之外的异常继续传播。测试不应接受逻辑错误时，应指定 `std::runtime_error`。
callable 采用借用方式，支持不可复制对象。`test_support_tests` 独立验证这些契约。
