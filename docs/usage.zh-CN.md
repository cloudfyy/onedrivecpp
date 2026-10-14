# 使用与命令输出

[English](usage.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 输出、日志与自动化

不带参数运行或使用 `--help`（包括子命令帮助）时，用法说明顶部显示应用名称和
构建版本。`--version` 仅输出应用名称和版本号。

上传复用下载已有的共享传输统计和输出后端。Console 增加 `UL`／`Upload done`
进度；TUI 分别保留上传、下载面板，并显示扫描、传输、应用变更、等待阶段。
Monitor 每轮结束后回到等待，不会被当成监控任务已退出；有阻止或失败的轮次在
等待时保留未完成提示。

JSON 保留原有 `download_progress` 和 `execution_summary` 格式，新增
`upload_progress`，包含 `completed_files`、`file_count`、`uploaded_bytes`、
`total_bytes`、`percentage`、`completed`、`bytes_per_second`、
`estimated_seconds_remaining` 和 `elapsed_milliseconds`。上传总量未知时，
`percentage` 为 `null`，不会伪造 0%／100%。`sync_context` 提供配置的 `direction`
和 `preview` 标记；`sync_phase` 为 `preparing`、`scanning`、`scanning_local`、
`downloading`、`uploading`、`applying` 或 `waiting`。消费者应忽略不需要的新增事件；
`--quiet` 会抑制上述进度、上下文和阶段记录。

累计进度可包含恢复偏移，但速度只统计本次操作新增的有效载荷网络字节，上传重传
也计入传输量，历史恢复偏移或复用的本地下载文件不计入。上传文件须通过远端结果
验证并提交本地状态后才算完成；单方向 100% 不代表同步最终成功，应结合退出码和
最终摘要判断。停止或失败时保留可恢复的日志，不伪装成传输完成。

CLI、GUI 和同用户 systemd service 共用状态目录运行锁。Monitor 在整个生命周期
（包括等待）持锁，停止须等 worker 和清理结束才释放。同步／预览、Monitor、
单文件下载及部分文件清理还会登记实际账号／Drive 同步目录：不同状态目录不能绕过
相同或父子重叠目录的占用检查，独立的兄弟 Drive 目录仍可并行。路径规范化及设备／
inode 身份用于识别别名，符号链接继续被拒绝。预检只校验同步路径，不提前创建或
修改目录；同步取得实际目录占用锁后才准备目录。

登记位置固定为 `/tmp/onedrive-cpp-locks-<有效 UID>`，目录权限为 `0700`，文件为
`0600`。参与进程须共享本机 `/tmp` 命名空间，不要为此 service 启用
`PrivateTmp=true`。这是当前 Linux 用户、本机内的互斥，不覆盖跨用户或跨主机；
不参与登记的其他工具及旧客户端仍需自行停止。

占用时任务失败，不自动转发或排队。`runtime_busy` 诊断提供资源、请求目录、持有者
PID、操作、启动时间和状态／同步目录；JSON 通过既有 section 的字符串 `values`
及错误消息输出，元数据不包含认证凭据。占用以 OS 锁为准，不凭 PID 是否存在判断。
崩溃自动释放锁，下一次登记回收陈旧目录记录；不要删除锁文件来解除活跃任务占用，
应停止持有者。

运行诊断默认以 `info` 级别写入标准错误。systemd 会自动收集该输出：

```bash
journalctl --user -u onedrive-cpp.service -f
```

每个子命令都支持诊断日志和用户输出选项：

```bash
onedrive-cpp transfer sync --dry-run --log-level debug
onedrive-cpp transfer watch --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
onedrive-cpp transfer sync --dry-run --color always
onedrive-cpp transfer sync --ui tui
onedrive-cpp transfer sync --theme ocean
onedrive-cpp account login --ui tui
onedrive-cpp inspect health --ui tui
onedrive-cpp inspect status --ui tui
onedrive-cpp inspect drives --ui tui
onedrive-cpp inspect shared --ui tui
onedrive-cpp inspect sites Engineering --ui tui
onedrive-cpp inspect quota --ui tui
onedrive-cpp transfer download Documents/report.pdf --ui tui
onedrive-cpp transfer sync --dry-run --output json
onedrive-cpp transfer sync --dry-run --quiet
onedrive-cpp inspect drives --output json
onedrive-cpp inspect shared
onedrive-cpp inspect sites Engineering
onedrive-cpp inspect quota
onedrive-cpp inspect status
onedrive-cpp inspect blocked
onedrive-cpp inspect blocked --reason unsupported_onenote --output json
onedrive-cpp inspect blocked --search Documents --ui tui
onedrive-cpp inspect storage
onedrive-cpp inspect partials
onedrive-cpp inspect files Documents --status modified
onedrive-cpp inspect verify Documents --mode content
onedrive-cpp inspect config
onedrive-cpp state cleanup --dry-run
onedrive-cpp state cleanup --yes
onedrive-cpp state migrate --dry-run
onedrive-cpp state migrate --yes
```

日志级别支持 `trace`、`debug`、`info`、`warn`、`error`、`critical` 和
`off`，表示后端诊断日志的最低级别：`--log-level warn` 保留警告、错误和严重日志。
它不筛选命令结果；可用 `--quiet` 隐藏较低级别的结果消息。TUI 活动列表还会额外
隐藏跟踪／调试／信息级别的后端详情，即使这些日志已启用并写入文件。
日志文件达到 5 MiB 时会轮转，并保留三个旧文件。父目录必须已经存在，
由当前用户所有，路径中不能含有符号链接，也不能允许其他用户写入。当前日志和
轮转文件都必须是当前用户拥有的普通单硬链接文件；客户端会强制使用 `0600`
权限，并拒绝符号链接和多硬链接文件。

日志不会记录认证 token、设备代码、Authorization header、代理密码或预授权传输
URL，但仍可能包含账号与 Drive 显示名称、远端项目名称和本地路径。复制或提交
诊断日志时，请继续按敏感数据保管，不要放宽客户端设置的私有权限。

启用调试日志后，下载校验诊断会记录远端项目 ID、eTag、预期与实际大小、初始断点
与持久化检查点偏移、哈希计算方式，以及远端与本地哈希。流式哈希校验失败时，
客户端会在删除临时文件前，从磁盘重新计算 SHA-256 和 QuickXorHash，并记录是否
与流式结果一致。磁盘复算失败会单独记录，但不会替换原始校验错误。这些诊断
不会关闭校验或接受校验失败的下载。新增诊断全部使用 `debug` 级别，原有校验
失败消息仍为警告。复现前请将日志配置改为 `debug`。成功下载也会记录相同校验信息。

`--color=auto` 是默认值：仅在终端中启用样式；设置 `NO_COLOR` 后会自动禁用。
`always` 强制输出 ANSI 样式，`never` 则始终禁用。`--output=json` 每行输出
一个紧凑的 JSON 对象，绝不包含 ANSI 序列。在该模式下执行危险操作时，必须
使用 `--yes` 显式确认。应用消息和后端日志共用六个级别；成功结果使用 `info`，
不再单列 `success`。JSON 保留原始级别：`trace`、`debug`、`info`、`warning`、
`error`、`critical`。`--quiet` 隐藏跟踪、调试和普通信息，但仍保留警告、错误和
严重消息。诊断日志继续写入标准错误，命令结果写入标准输出。

`account login`、全部 `inspect` 查询命令和全部 `transfer` 命令读取配置中的 `console.ui`，默认值为 `auto`；
命令行显式提供的 `--ui` 只覆盖本次运行。auto 模式只有在标准输入和标准输出均连接终端、`TERM`
支持终端控制，并且窗口至少为 60 列 × 12 行时，才会打开 FTXUI 实时状态面板。
输出重定向、管道、JSON、quiet 模式、`TERM=dumb` 或窗口过小时，会自动切回普通
Console。检查类命令的 TUI 会保留最终结果，统一显示 `Press Enter to exit (or q)`。
按 Enter、`q` 或 `Q` 即时退出，`q` 和 `Q` 无需再按回车；其他按键不会关闭结果页。
运行中的同步、传输和登录流程的按键行为保持不变。
小窗口裁剪详情时也会保留退出提示；可扩大终端或使用 Console 查看全部字段。
Console 和 JSON
仍立即退出。使用 `--ui=console` 可在终端历史中保留结果而无需等待按键。
`--ui=tui` 则会强制启用面板，
若当前终端不支持，会明确报错而不是静默回退。在 watch 面板中按 `q`、`Q` 或
`Esc` 可以安全退出。

面板启动后会立即进入终端的全屏备用缓冲区，标题显示客户端版本，退出时恢复原屏幕。
download 面板复用同步期间持久显示的进度、传输速率和 ETA。同步中断后重试时，
文件数和字节进度会包含已经完成并验证的下载，速度与 ETA 则只计算本轮新传输。
界面状态使用“云端”“文件”等用户化措词，不直接展示底层 API 名称。`console.theme`
可选择默认的荧光绿 `hacker`、青蓝 `ocean`、琥珀 `amber` 或洋红/青色
`synthwave`；`--theme` 只覆盖本次运行。`--color=never` 和 `NO_COLOR` 会保留
所选布局，但关闭主题颜色。
命令帮助只显示名称选项，不展示内部枚举编号：

```text
--ui {auto,console,tui} [auto]
--theme {hacker,ocean,amber,synthwave} [hacker]
```

## 只读账号与同步信息

`inspect drives` 列出当前 Microsoft 账号可用的 OneDrive Drive，并标记配置正在使用的
Drive。`reference` 单独显示配置中的引用（例如 `me`），同时保留真实 Drive ID；
其他 Drive 不会误标为 `me`。每个 Drive 显示总容量、已用、剩余、回收站占用及配额
状态，复用 `inspect quota` 的格式；缺失配额信息显示 `unavailable`。

TUI 每页显示一个 Drive，查询完成后从第一页开始，并显示 `Drive 1/3` 这样的页码。
按左右方向键或 `p`/`n` 切换，无需再按 Enter；首尾不循环。
按 Enter、`q` 或 `Q` 退出。Console 和 JSON 仍完整输出全部 Drive，不分页、不等待按键。

文件统计明确标注 `local state (not cloud totals)`，不遍历云端：
`known_files` 对已保存的文件快照、待下载、断点下载和未删除的 blocked item 按文件
ID 去重，排除目录。`downloaded_files` 表示受跟踪且仍存在于本地 Drive 根目录内的
普通文件，包括本地修改过或曾上传的文件；这是已落盘数量，不是历史下载次数或内容
校验结果。`pending_files` 对待下载和断点记录去重，`blocked_files` 统计未删除的
blocked 文件。不同状态可重叠，例如已有本地文件仍可能有待下载更新。
各 Drive 分别只读查询自己的状态库；缺少状态库显示 `unavailable`，不会伪装为零。
数据库异常或文件系统检查失败会明确报错。

`inspect shared` 会列出 `sharedWithMe` 返回的项目，以及已添加到当前 OneDrive 的
快捷方式（shortcut）；同时显示配置共享 Drive 所需的目标 Drive ID 和 item ID。
`inspect sites QUERY` 搜索可访问的 SharePoint 站点并列出各站点的文档库 Drive。
站点发现要求 `Sites.Read.All` 或 `Sites.ReadWrite.All`；修改 scope 后需要重新
运行 `onedrive-cpp account login`。

`inspect quota` 显示配置 Drive 的总量、已用、剩余、回收站占用和配额状态。`inspect status`
合并当前账号及规范 Drive 身份与本地只读状态，包括同步模式、删除策略、最后一次
同步结果、tracked/blocked 数量、pending journal、Delta cursor、
selective-sync fingerprint 和 WebSocket 配置。

`inspect blocked` 完全离线读取当前账号、配置 Drive 中与 GUI 相同的持久化阻止项。
不要求应用 ID 或刷新令牌，不创建状态目录／数据库、不获取同步运行锁，也不启动传输。
每项显示路径、原因代码／详情、Drive／item ID、累计发现次数、策略控制方式及共享策略
是否允许恢复。发现次数不是下载重试次数；此命令不能更改策略或解除阻止。
`--reason CODE` 精确筛选原因代码，允许查询未知／未来代码；`--search TEXT`
同时搜索路径、原因代码和详情，忽略 ASCII 大小写，其他 UTF-8 字节按原样匹配。
两个筛选可组合，摘要分别显示已记录总数和匹配数。
读取现有数据库时，SQLite 可能创建常规 WAL 协调文件，但不更改已保存记录或游标。
缺少账号／Drive／数据库时显示空结果；损坏、有歧义或不安全的状态以及数据库错误
明确报错，不伪装为成功的空列表。JSON 为每个匹配项输出 `blocked_item`，
最后输出 `blocked_item_summary`，沿用标准的字符串字段 `values` 对象；
空结果还输出 `no_blocked_items`。查询到阻止项仍正常返回 0，不表示同步失败。
TUI 分页保留各项详情，左右方向键或 `n`／`p` 翻页，Enter／`q` 退出；
终端过小导致详情被裁剪时可改用 Console 或 JSON。

`inspect storage` 显示解析后的 Drive 数据目录、文件系统容量和可用空间、tracked
文件字节数、partial download 占用以及状态数据库路径。`inspect partials` 列出每个
partial download，并将其标记为可续传、缺失、类型变化、位于同步根之外、路径不匹配
或 checkpoint 无效。TUI 顶部保留记录数、可续传数、异常数和本地临时文件大小合计，
下面显示完整文件列表，包括远端路径、状态和已保存的下载进度 / 文件总大小。
上下方向键或 `k`/`j` 选择文件，
列表自动滚动以显示选中项，首尾不循环。终端至少 18 行时，下方同时显示选中文件的
远端路径、目标路径、临时路径和本地临时文件大小；更小的窗口优先保留列表与操作提示。
路径过长被裁剪时，可扩大终端或使用 Console 查看完整信息。
Enter、`q` 或 `Q` 退出；Console 和 JSON 仍完整输出每条记录。

三个大小字段的区别：

- `saved download progress`（已保存的下载进度）：程序上次记住已经下载了多少。
- `total file size`（文件总大小）：记录中保存的云端文件完整大小，不是剩余下载量。
- `local temporary file size`（本地临时文件大小）：未下载完的临时文件现在有多大，
  不是云端文件大小，也不是最终目标文件大小。

下载进度不是每写入一点数据就立即保存，因此它有时会与临时文件现在的大小不同。
`unavailable` 表示未能确认大小，不等于零。JSON 字段名和数值保持不变。

`inspect files [PATH]` 使用保存的大小和修改时间检查 tracked
普通文件；可用 `--status ok|missing|modified|type-changed|outside-root` 过滤输出。

这些命令不会同步文件或修改远端内容。检查本地状态的命令以只读方式打开现有 SQLite
数据库，不会创建、迁移、修复或隔离缺失或旧版数据库。全部检查命令均支持
`--output json`。

`inspect health` 递归检查配置状态目录中的全部 `items.sqlite3`，包括账号目录布局之外
的旧数据库。每项结果显示实际版本 `schema_version` 和程序支持的最新版本
`latest_schema_version`（Console/TUI 标签为 `version` 和 `latest`）。无法读取版本时
显示 `unknown`。完整性检查通过的旧库显示 `upgrade-required`，不再直接标记为
`unhealthy`；不支持的版本、完整性或结构检查失败仍为不健康。任一数据库需要升级或
检查失败时，命令返回 1。

`inspect verify [PATH]` 会执行与 `inspect files` 相同的元数据检查。指定
`--mode content` 后，还会计算状态中保存的 QuickXorHash、SHA-1 或旧版 SHA-256，并报告
`verified`、`hash-mismatch` 或 `hash-unavailable`。经过验证的下载和远端元数据更新
会把内容哈希保存到 item 状态；旧 tracked 文件可能要等后续同步记录哈希后才能做
内容验证。

`inspect config` 显示最终生效的非敏感路径、同步策略、传输限制和输出设置，并只说明
代理是否配置。它不会输出应用凭据、认证 token、代理凭据或代理 URL。

`state cleanup --dry-run` 预览当前账号、当前配置 Drive 的下载残留，不清理整个 Drive
目录或下载完成的文件。配置中的 `sync.dry_run = true` 同样阻止实际清理，`--yes`
不能绕过它。非预演模式使用 `--yes` 确认删除；否则文本模式要求输入配置的 Drive 引用，
JSON 模式必须提供 `--yes`。

可续传的文件会保留，包括文件大小超过已保存进度的情况。临时文件已经丢失时只清理
记录；文件被截短、保存的总大小或进度无效时，在路径安全的前提下一次清理文件和记录。
孤立文件必须严格符合 `.<文件名>.onedrive-partial-<PID>-<序号>`，后两项必须是无前导零
的正十进制数，并且没有被已跟踪文件及未完成的下载、上传、删除、移动操作引用。

越界路径、符号链接、非普通文件、多硬链接、非当前用户所有的文件、引用不明确或名称
不符合规则的记录，均保留文件及记录并报告跳过。目录扫描不完整时，在删除前终止。
删除前再次核对文件是否与预览时一致，文件删除落盘后才移除记录；删除失败保留记录。
中断后可重跑，已删除文件对应的记录会按“文件已丢失”继续处理。

预览显示保留项、仅删记录数、待删文件数、文件大小合计和跳过数；完成后显示实际删除的
记录、文件、字节数以及跳过和失败数。只要有跳过或失败，就返回退出码 1，不会把部分成功
说成全部成功。字节数是文件逻辑大小，不保证等于磁盘实际释放空间。只清孤立文件时，
不会创建原本不存在的数据库。清理同时持有状态目录锁和实际同步目录占用锁；
清理前仍须停止可能写入同一数据目录、不参与锁登记的其他工具。

## 离线升级数据库

`state migrate --dry-run` 列出已有数据库及当前、目标版本，不升级数据库或创建备份。
实际升级前请停止使用同一状态目录的 sync/watch 进程，然后使用与检查相同的
`--config` 运行 `state migrate --yes`。未指定 `--yes` 时，文本模式要求输入
`migrate` 确认；JSON 模式必须指定 `--yes`。配置中的 `sync.dry_run = true` 也会
阻止实际升级。

命令无需认证、不访问网络、不同步文件，并复用同步进程的运行锁。开始前检查全部发现
的数据库，发现损坏或不支持的版本时拒绝执行；已经最新的数据库直接跳过。旧库的完整
结构校验在迁移过程中执行，因此预览成功不保证迁移一定成功。

每个数据库升级前，使用 SQLite 在同目录生成私有、一致的备份
`items.sqlite3.pre-migrate-v<VERSION>-<SUFFIX>`，包含已提交的 WAL 数据，并输出备份
路径。备份需要额外磁盘空间，不自动删除，且可能包含敏感同步元数据；请保留至确认升级
成功后再处理。每个库的全部升级步骤及最终完整性、结构校验位于同一事务中：失败时
回滚该库、保留备份并以状态 1 停止；本轮先前已成功升级的其他数据库仍保持已升级。
此命令绝不自动重建或隔离损坏数据库。完成后可再次运行 `inspect health`。

## Shell 自动补全

安装包会安装 Bash 和 Zsh 补全定义，支持子命令、选项、枚举值和文件路径。
新终端启用 Shell 补全后会自动加载这些定义。

开发构建可在当前 Bash 中执行：

```bash
source packaging/completions/onedrive-cpp.bash
```

随后可以使用 Tab 补全：

```bash
build/release/onedrive-cpp <Tab>
build/release/onedrive-cpp transfer sync --<Tab>
build/release/onedrive-cpp transfer sync --log-level <Tab>
```

安装位置分别为 `share/bash-completion/completions/onedrive-cpp` 和
`share/zsh/vendor-completions/_onedrive-cpp`。

## 手册页

CMake 安装规则会把英文 section 1 手册安装到标准的 `share/man/man1` 目录，
并把简体中文版安装到 `share/man/zh_CN/man1`。安装 DEB 包后，`man` 会按照
当前 locale 自动选择语言：

```bash
man onedrive-cpp
LANG=zh_CN.UTF-8 man onedrive-cpp
```

Debian 的 `man-db` trigger 会自动更新索引。直接使用 `cmake --install` 安装到
自定义前缀后，如有需要可手动刷新本地索引：

```bash
sudo mandb
```

不安装也可以查看构建目录中生成的手册：

```bash
man --local-file build/release/generated/onedrive-cpp.1
```
