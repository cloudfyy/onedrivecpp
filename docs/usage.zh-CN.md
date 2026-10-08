# 使用与命令输出

[English](usage.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 输出、日志与自动化

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
`off`。日志文件达到 5 MiB 时会轮转，并保留三个旧文件。父目录必须已经存在，
由当前用户所有，路径中不能含有符号链接，也不能允许其他用户写入。当前日志和
轮转文件都必须是当前用户拥有的普通单硬链接文件；客户端会强制使用 `0600`
权限，并拒绝符号链接和多硬链接文件。

日志不会记录认证 token、设备代码、Authorization header、代理密码或预授权传输
URL，但仍可能包含账号与 Drive 显示名称、远端项目名称和本地路径。复制或提交
诊断日志时，请继续按敏感数据保管，不要放宽客户端设置的私有权限。

`--color=auto` 是默认值：仅在终端中启用样式；设置 `NO_COLOR` 后会自动禁用。
`always` 强制输出 ANSI 样式，`never` 则始终禁用。`--output=json` 每行输出
一个紧凑的 JSON 对象，绝不包含 ANSI 序列。在该模式下执行危险操作时，必须
使用 `--yes` 显式确认。`--quiet` 会隐藏普通信息和成功消息，但仍保留警告和
错误。诊断日志继续写入标准错误，命令结果写入标准输出。

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
`--mode content` 后，还会计算状态中保存的 Graph SHA-256 或 QuickXorHash，并报告
`verified`、`hash-mismatch` 或 `hash-unavailable`。经过验证的下载和远端元数据更新
会把内容哈希保存到 item 状态；旧 tracked 文件可能要等后续同步记录哈希后才能做
内容验证。

`inspect config` 显示最终生效的非敏感路径、同步策略、传输限制和输出设置，并只说明
代理是否配置。它不会输出应用凭据、认证 token、代理凭据或代理 URL。

`state cleanup --dry-run` 只列出无效 partial download 记录和孤立的
`.onedrive-partial-*` 普通文件。使用 `state cleanup --yes` 才会实际清理。位于解析后
Drive 根目录之外、名称不符合目标 partial 规则、符号链接或非普通文件绝不会被删除；
但相应的无效数据库记录仍可移除，防止后续错误续传。

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
