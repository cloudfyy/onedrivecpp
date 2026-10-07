# 同步与恢复

[English](synchronization.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 状态重置与 Delta 重建

使用以下命令重置当前配置 Drive 保存的 `deltaLink`：

```bash
onedrive-cpp reset-state
```

该命令保留认证 token、配置、同步目录中的本地文件、item 快照、pending
download、pending upload 和 pending move 恢复记录、选择性保留副本的上传抑制、
blocked item 以及其他 Drive 的状态。下一次 `sync` 会先恢复 pending 操作，再
执行完整的初始 Delta 查询。同步过程中继续使用旧快照安全判断本地文件是否被
修改，并用完整远端清单替换当前 Drive 的旧 item 元数据。

如果需要丢弃当前配置 Drive 的全部同步状态，必须显式使用危险模式：

```bash
onedrive-cpp reset-state --clear-all
```

命令要求准确输入当前配置的 Drive 引用（例如 `me`），确认后才会删除 item
快照、Delta 游标、pending download、partial download、pending upload、pending move 恢复记录、
选择性保留副本的上传抑制以及 blocked item。没有可用的配置引用时，改为要求输入
原始 Drive ID。本地文件和其他 Drive 的状态不会被修改。由于本地快照已被清除，下一次同步可能报告本地
修改冲突。自动化场景必须使用
`reset-state --clear-all --yes` 显式承担该风险；未指定 `--clear-all` 时
`--yes` 会被拒绝。

## 数据库完整性与修复

客户端会在启动时和 migration 完成后对每个 Drive 数据库执行 SQLite
`quick_check`，并验证所有业务表、列类型、`NOT NULL` 约束和主键索引是否与当前
程序生成的 schema 一致。确认损坏的数据库及其 WAL/SHM sidecar 会在原目录隔离
为 `items.sqlite3.corrupt-<时间戳>`，随后自动创建可重建的新状态库。版本为零但
并非空库、版本高于当前程序或物理结构不兼容不属于损坏，仍会在读取 item 状态前
停止。本地文件不会被删除，但重建后缺少旧快照，下一次同步可能报告本地修改冲突。

数据库连接会关闭 trusted schema 和扩展加载，启用 SQLite defensive 与外键模式，
限制解析器和数据库增长资源，为短暂锁竞争等待有限时间，并使用 WAL、FULL
synchronous、自动 checkpoint 和 FAST secure delete。状态目录保持 `0700`；
数据库及已有 WAL/SHM 必须是当前用户拥有的普通单硬链接文件，并限制为 `0600`。

无需连接 Microsoft Graph，即可对所有 Drive 数据库执行只读的完整
`integrity_check`、外键检查和精确 schema 检查：

```bash
onedrive-cpp doctor
```

任何数据库不健康时命令返回非零；可配合 `--output json` 输出结构化诊断。

## 单文件下载

无需执行整个 Drive Delta 同步即可下载单个文件：

```bash
onedrive-cpp download "Documents/report.pdf"
```

参数是 Drive 相对文件路径。程序会在发出 Graph 请求前拒绝绝对路径、空组件、
`.`、`..`、控制字节和反斜杠。命令按路径解析唯一 DriveItem，并复用常规下载的
eTag 前置条件、HTTPS 重定向策略、Range 分片、durable checkpoint 恢复、完整性
校验、磁盘空间预留、本地修改保护和原子安装。目录及带有 Graph `malware` facet
的文件会被拒绝。程序会更新该文件的 item 快照，但不会推进 Drive `deltaLink`。

使用 `download --dry-run` 可以显示解析后的远端路径、本地目标和预期大小，不会
创建 item 数据库或修改下载文件：

```bash
onedrive-cpp download "Documents/report.pdf" --dry-run
```

## Delta 同步与原子应用

`sync` 命令会刷新 OAuth access token，在 Microsoft 返回轮换后的 refresh
token 时安全持久化，并通过分页的 Microsoft Graph Delta 请求获取配置 Drive
中的递归文件树。首次成功查询会把远端元数据和最终 `deltaLink` 原子写入所选
账号和 Drive 的 `items.sqlite3`；后续运行复用该链接，只获取新增、修改和
删除的项目。仅当所有分页均成功处理后才推进 `deltaLink`，因此中途失败不会
丢失尚未应用的变更。

`--dry-run` 会查询远端变化并显示创建目录、下载文件、下载字节数和本地删除
数量，但不会创建文件或更新 SQLite 状态。普通模式会创建远端目录并下载新增
或修改的文件。下载先写入目标目录中的临时文件，完成大小校验和 `fsync` 后再
原子替换目标。每个并发 worker 会立即提交已完成文件，不会把整个批次都保留为
临时文件，因此后续独立下载失败不会丢弃已完成的传输。每个变化成功应用或在
同一 SQLite 事务中可靠记录为 blocked 后，程序才推进 `deltaLink`。系统性失败
时，已完成文件的本地快照会支持安全重试。

## 并发与冲突保护

每个下载从准备阶段到最终 ItemStore 更新都会持有按 `(drive_id, remote_id)`
索引的 operation-coordinator 租约。同一远端条目的操作会串行执行，不同条目和
Drive 仍保持并行。上传、删除和远端移动路径也获取同一租约，防止一个条目的
网络、文件系统和状态变更相互交错。

程序不会覆盖无法确认未被用户修改的本地文件。下载前会记录目标是否存在、大小、
修改时间和 SHA-256 指纹，并在写 journal 前及原子替换前立即复核。下载期间创建
或修改的文件会被保留并持久化为 `local_modification` blocked item。非法远端
路径、符号链接、本地路径类型冲突、带有 Microsoft Graph `malware` facet 的文件
以及被阻塞目录的子项也会持久化为 blocked item。被标记为恶意的文件不会下载，
也不能替换已有本地数据。其他独立文件继续同步；游标安全推进后 `sync` 返回状态码
2。后续每次增量同步都会自动重试 blocked item，成功或远端删除后清除记录。认证、
Graph、数据库、同步根目录权限、整体磁盘容量和下载传输错误仍然是致命错误。

## 移动与删除

Delta 项目保留相同远端 ID 但路径变化时，客户端会在不覆盖已有目标的前提下，
在本地执行重命名或移动。Graph 只报告目录本身时，也会重新映射所有已跟踪后代。
当一个移动的目标由另一个待移动项目占用时，会先执行腾空目标的移动；父目录移动
后，显式子项重命名也会使用重新映射后的实际源路径。名称交换和其他依赖环会通过
私有隐藏 staging 路径安全打破，并使用持久 filesystem identity 恢复任意中断点。
内容未变化的文件无需重新下载；远端内容同时变化时则在移动后下载。移动前会将
源路径、目标路径、可选 staging 路径和源 device/inode 写入 SQLite pending-move
journal，项目状态、Delta 游标和 journal 清理在同一事务中提交。恢复时只认领
身份完全匹配的对象。
跨文件系统移动记录为 `cross_device_move`，不会执行复制后删除。

远端删除记录只有在本地普通文件的大小和修改时间仍与可信同步快照一致时，才会
移除该文件。本地目标已经不存在时会直接清理快照。目录按子项优先顺序处理，而且
只有为空时才会删除，因此不会递归删除未跟踪的本地内容。本地已修改文件、符号
链接、意外路径类型和非空目录都会被保留并记录为可重试 blocked item；即使
`sync.local_conflict = "backup"` 也不会自动备份后删除。Delta 游标推进后，后续
同步仍会重试。完整 Delta 刷新会用 Graph 完整清单与旧快照对账，因此重置或失效
游标不会漏掉远端删除。仅因 `sync_list` 排除的项目会移除同步快照并保留本地
文件，同时用持久 filesystem identity 抑制该保留对象从旧路径重新上传。

## 完整性、Journal 与断点恢复

Microsoft Graph 提供文件内容哈希时，程序会在临时文件进入待安装 journal 前
进行校验：优先使用 SHA-256，否则校验 OneDrive/SharePoint QuickXorHash。
从 byte 0 开始的下载会在写入数据时增量计算 SHA-256 和 QuickXorHash，并将
流式 SHA-256 同时用于崩溃恢复指纹。断点续传或重试期间字节 offset 不连续时，
会安全回退到对完整文件重新计算哈希。哈希不匹配会删除 partial 检查点和临时
文件，使下次尝试从 byte 0 重新下载。本地 SHA-256 指纹仅用于保护崩溃恢复
状态，不能替代远端完整性哈希。

安装后的文件采用 Graph 权威的 `fileSystemInfo.lastModifiedDateTime`；缺少有效
权威时间的文件会在下载前被拒绝。新下载文件的权限由 `0666` 和进程 `umask`
共同决定（`umask 0022` 时通常为 `0644`），程序不会添加可执行位。

下载完成后，程序先把临时路径、目标路径、远端元数据、大小和 SHA-256 内容
指纹写入 SQLite `pending_download` journal，再执行原子替换。程序重启时会先
恢复 journal，因此 SQLite 是崩溃恢复的权威来源，不依赖目标文件系统的
扩展属性。

当 `sync.local_conflict = "backup"` 已保留本地内容时，待安装 journal 还会记录
持久备份路径及其 SHA-256 指纹。恢复流程只有在备份和当前目标都与该指纹匹配时，
才会替换仍然存在的目标；备份缺失、被修改或变成符号链接时会停止恢复，而不会
冒险覆盖本地数据。

大文件每个 Range 分片成功并完成 `fsync` 后，还会单独持久化 SQLite
`partial_download` 检查点，其中记录远端 ETag、预期大小、目标、临时路径和已
可靠写入的字节数。只有 HTTP 状态、`Content-Range`、总大小和实际接收字节数
与请求范围完全一致时才接受分片；被拒绝的分片会回滚到前一个可靠 offset。
后续进程仅在检查点元数据和同目录普通文件仍匹配时续传；未形成检查点的尾部
字节会被截断，远端版本变化或陈旧状态则从 byte 0 重新开始。完整下载会先从
partial 状态转换到现有的待安装 journal，再进行原子替换。

Delta 查询期间，文本输出和日志会在每页处理完成后显示已完成页数和累计扫描
条目数。JSON 输出会产生包含 `pages`、`items` 和 `completed` 字段的
`delta_progress` 事件。Microsoft Graph 不会预先提供 Delta 条目总数，因此无法
显示准确百分比。`--quiet` 会隐藏控制台进度，但不会隐藏已配置的日志输出。

创建目录或下载文件之前，同步会验证每个远端路径。程序会拒绝空路径、绝对
路径、`.`/`..` 路径段、NUL、控制字节、空路径段，并根据目标文件系统限制检查
单个名称和完整路径的字节长度。错误会指出远端路径、具体问题组件、实际长度和
支持上限；控制字节会被转义，确保诊断信息可以安全显示。非法名称会保存为
blocked item，其他文件继续同步，并通常需要在 OneDrive 中重命名。

下载进度会汇总所有并发传输。文本输出显示已完成/总文件数、总体字节百分比，
以及自动使用 B、KiB、MiB 或 GiB 的传输容量。交互式终端只原位刷新一条简短
进度；重定向文本和 JSON 输出每增加一个百分点产生一条事件。JSON 事件保留精确
字节数，并包含 `completed_files`、`file_count`、`downloaded_bytes`、
`total_bytes`、`percentage` 和 `completed` 字段。`--quiet` 会隐藏进度输出。

如果 Microsoft Graph 以 `410 Gone` 拒绝已保存的 Delta 游标，同步会自动改用
完整 Delta 查询重试。已有本地快照会继续用于冲突检测；只有完整同步计划成功后，
程序才会替换已保存的游标和远端 item 清单。

`filesystem.metadata` 控制是否额外写入 `user.*` xattr：

```toml
[filesystem]
metadata = "auto"
```

- `auto`：实际创建探测文件验证 xattr；支持时写入辅助标记，不支持时自动使用
  纯 database journal。
- `xattr`：要求 xattr 支持，探测失败时同步立即停止。
- `database`：完全不读写 xattr，适合 FUSE、SMB/NFS 或其他扩展属性语义不稳定
  的文件系统。

能力判断基于目标同步目录中的实际读写探测，而不是文件系统名称白名单。

## 重试策略与长驻服务

Microsoft Graph 分页请求、下载重定向、文件下载和 Range 分片请求会重试 HTTP
408、429、502、503 和 504 响应。客户端会遵循数值形式的 `Retry-After`
响应头；响应头缺失或无效时使用有上限的指数退避。如果预认证下载 URL 返回
HTTP 401 或 403，客户端会从 Graph 获取新的 redirect 并重试一次，且不会把
Graph bearer token 发给下载主机。失败的 Range 请求会先把临时文件回滚到当前
分片边界再重试。重试次数受到限制，持续服务故障会明确失败，而不是无限等待。

可在配置文件中调整节流策略：

```toml
[graph.throttle]
maximum_retries = 4
initial_delay_seconds = 1
maximum_delay_seconds = 300
```

当可重试响应没有有效的数值 `Retry-After` 时，初始等待时间会在每次重试后
加倍，直至配置的最大值。如果服务器要求的等待时间超过配置上限，客户端会
明确失败，而不是意外长时间休眠。

安装 DEB 后启用用户服务：

```bash
systemctl --user daemon-reload
systemctl --user enable --now onedrive-cpp.service
journalctl --user -u onedrive-cpp.service -f
```

用户服务会运行长期驻留的 monitor：启动时同步一次，通过 inotify 监听本地
Drive 目录，按配置周期轮询 Graph，并在意外失败后自动重启。
`systemctl --user stop` 会发送 `SIGTERM`，使其安全退出。
