# 同步与恢复

[English](synchronization.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 状态重置与 Delta 重建

使用以下命令重置当前配置 Drive 保存的 `deltaLink`：

```bash
onedrive-cpp state reset-cursor
```

该命令会保留认证 token、配置、同步目录中的本地文件、item 快照、pending
download、pending upload 和 pending move 恢复记录、对选择性保留副本的上传
抑制、blocked item，以及其他 Drive 的状态。下一次 `transfer sync` 会先恢复 pending
操作，再执行完整的初始 Delta 查询。同步期间，程序仍会使用旧快照安全判断本地
文件是否经过修改，并用完整的远端清单替换当前 Drive 的旧 item 元数据。

如果需要丢弃当前配置 Drive 的全部同步状态，必须显式使用危险模式：

```bash
onedrive-cpp state clear
```

命令会要求准确输入当前配置的 Drive 引用（例如 `me`）。确认后，程序才会删除
item 快照、Delta 游标、pending download、partial download、pending upload、
pending move 恢复记录、对选择性保留副本的上传抑制，以及 blocked item。如果
没有可用的配置引用，程序会改为要求输入原始 Drive ID。本地文件和其他 Drive
的状态不会改变。

由于本地快照已被清除，下一次同步可能报告本地修改冲突。自动化场景必须使用
`state clear --yes` 显式承担该风险；`--yes` 只适用于 `state clear`。

## 数据库完整性与修复

客户端会在启动时以及 migration 完成后，对每个 Drive 数据库执行 SQLite
`quick_check`。它还会验证所有业务表、列类型、`NOT NULL` 约束和主键索引是否
与当前程序生成的 schema 一致。确认数据库损坏后，程序会将数据库及其 WAL/SHM
sidecar 隔离到原目录，并命名为 `items.sqlite3.corrupt-<时间戳>`，随后自动创建
可重建的新状态库。

版本为零但并非空库、版本高于当前程序，或物理结构不兼容，都不属于数据库损坏；
遇到这些情况时，程序仍会在读取 item 状态前停止。本地文件不会被删除，但重建后
缺少旧快照，因此下一次同步可能报告本地修改冲突。

数据库连接会关闭 trusted schema 和扩展加载，启用 SQLite defensive 与外键模式，
限制解析器和数据库增长资源，为短暂锁竞争等待有限时间，并使用 WAL、FULL
synchronous、自动 checkpoint 和 FAST secure delete。状态目录保持 `0700`；
数据库及已有 WAL/SHM 必须是当前用户拥有的普通单硬链接文件，并限制为 `0600`。

无需连接 Microsoft Graph，即可对所有 Drive 数据库执行只读的完整
`integrity_check`、外键检查和精确 schema 检查：

```bash
onedrive-cpp inspect health
```

任何数据库不健康时命令返回非零；可配合 `--output json` 输出结构化诊断。

## 单文件下载

无需执行整个 Drive Delta 同步即可下载单个文件：

```bash
onedrive-cpp transfer download "Documents/report.pdf"
```

参数是相对于 Drive 的文件路径。发出 Graph 请求前，程序会拒绝绝对路径、空
组件、`.`、`..`、控制字节和反斜杠。命令会按路径解析出唯一的 DriveItem，并
复用常规下载所采用的 eTag 前置条件、HTTPS 重定向策略、Range 分片、durable
checkpoint 恢复、完整性校验、磁盘空间预留、本地修改保护和原子安装。目录以及
带有 Graph `malware` facet 的文件会被拒绝。程序会更新该文件的 item 快照，但
不会推进 Drive `deltaLink`。

使用 `download --dry-run` 可以显示解析后的远端路径、本地目标和预期大小，不会
创建 item 数据库或修改下载文件：

```bash
onedrive-cpp transfer download "Documents/report.pdf" --dry-run
```

## Delta 同步与原子应用

`transfer sync` 命令会刷新 OAuth access token。Microsoft 返回轮换后的 refresh token
时，程序会将其安全持久化。随后，命令通过分页的 Microsoft Graph Delta 请求
获取配置 Drive 中的递归文件树。首次查询成功后，程序会将远端元数据和最终的
`deltaLink` 原子写入所选账号与 Drive 的 `items.sqlite3`。后续运行会复用该
链接，只获取新增、修改和删除的项目。程序仅在成功处理全部分页后才推进
`deltaLink`，因此中途失败不会丢失尚未应用的变更。

`--dry-run` 会查询远端变化，并显示待创建的目录、待下载的文件、下载字节数和
本地删除数量，但不会创建文件或更新 SQLite 状态。普通模式会创建远端目录，并
下载新增或修改的文件。下载内容先写入目标目录中的临时文件；完成大小校验和
`fsync` 后，程序才会原子替换目标。

每个并发 worker 都会立即提交已经完成的文件，不会把整个批次都留在临时状态。
因此，即使后续某个独立下载失败，已完成的传输也不会被丢弃。只有在每项变化都
成功应用，或在同一个 SQLite 事务中可靠记录为 blocked 后，程序才会推进
`deltaLink`。如果发生系统性失败，已完成文件的本地快照可用于安全重试。

## 并发与冲突保护

每次下载从准备阶段开始，直至最终更新 ItemStore，都会持有按
`(drive_id, remote_id)` 索引的 operation-coordinator 租约。针对同一远端条目
的操作会串行执行；不同条目和 Drive 之间仍可并行。上传、删除和远端移动流程
也会获取同一租约，防止同一条目的网络操作、文件系统操作和状态变更相互交错。

如果无法确认本地文件未被用户修改，程序就不会覆盖它。下载前，程序会记录目标
是否存在，以及目标的大小、修改时间和 SHA-256 指纹；写入 journal 前和原子替换
前，还会立即复核。下载期间创建或修改的文件会被保留，并持久化为
`local_modification` blocked item。

非法远端路径、符号链接、本地路径类型冲突、带有 Microsoft Graph `malware`
facet 的文件，以及被阻塞目录的子项，也会持久化为 blocked item。程序不会下载
被标记为恶意的文件，也不会让它替换现有本地数据。其他独立文件会继续同步；
游标安全推进后，`transfer sync` 返回状态码 2。此后的每次增量同步都会自动重试 blocked
item；操作成功或远端项目删除后，记录会被清除。认证、Graph、数据库、同步根
目录权限、整体磁盘容量和下载传输错误仍属于致命错误。

## 移动与删除

如果 Delta 项目的远端 ID 不变、路径发生变化，客户端会在不覆盖现有目标的前提
下，在本地执行重命名或移动。即使 Graph 只报告目录本身，程序也会重新映射所有
已跟踪后代。如果某个移动目标被另一个待移动项目占用，程序会先执行能够腾空目标
的移动。父目录移动后，显式的子项重命名也会使用重新映射后的实际源路径。

对于名称交换和其他依赖环，程序会借助私有的隐藏 staging 路径安全打破依赖，
并利用持久的 filesystem identity 从任意中断点恢复。内容未变化的文件无需重新
下载；如果远端内容也发生变化，则在移动后下载。

移动前，程序会将源路径、目标路径、可选 staging 路径和源 device/inode 写入
SQLite pending-move journal。项目状态、Delta 游标和 journal 清理会在同一事务
中提交。恢复时，程序只会认领身份完全匹配的对象。跨文件系统移动会记录为
`cross_device_move`，不会执行“复制后删除”。

处理远端删除记录时，只有本地普通文件的大小和修改时间仍与可信同步快照一致，
程序才会删除该文件。如果本地目标已经不存在，则直接清理快照。目录按子项优先
的顺序处理，并且仅在为空时删除，因此不会递归删除未跟踪的本地内容。

本地已修改文件、符号链接、意外的路径类型和非空目录都会保留，并记录为可重试
的 blocked item。即使 `sync.local_conflict = "backup"`，程序也不会先自动
备份再删除。Delta 游标推进后，后续同步仍会继续重试。

完整 Delta 刷新会使用 Graph 完整清单与旧快照对账，因此重置或失效游标不会
漏掉远端删除。对于仅因 `sync_list` 被排除的项目，程序会移除同步快照但保留
本地文件，同时使用持久的 filesystem identity，防止该保留对象从旧路径重新上传。

## 完整性、Journal 与断点恢复

Microsoft Graph 提供文件内容哈希时，程序会在临时文件进入待安装 journal 前
校验内容：优先使用 SHA-256，否则使用 OneDrive/SharePoint QuickXorHash。
对于从 byte 0 开始的下载，程序会在写入数据时增量计算 SHA-256 和
QuickXorHash，并将流式 SHA-256 同时用作崩溃恢复指纹。如果断点续传或重试期间
的字节 offset 不连续，程序会安全回退，重新计算完整文件的哈希。

如果哈希不匹配，程序会删除 partial 检查点和临时文件，使下次尝试从 byte 0
重新下载。本地 SHA-256 指纹仅用于保护崩溃恢复状态，不能替代远端完整性哈希。

安装后的文件采用 Graph 权威的 `fileSystemInfo.lastModifiedDateTime`；缺少有效
权威时间的文件会在下载前被拒绝。新下载文件的权限由 `0666` 和进程 `umask`
共同决定（`umask 0022` 时通常为 `0644`），程序不会添加可执行位。

下载完成后，程序会先将临时路径、目标路径、远端元数据、大小和 SHA-256 内容
指纹写入 SQLite `pending_download` journal，然后再执行原子替换。程序重启时
会先恢复 journal。因此，SQLite 是崩溃恢复的权威来源，不依赖目标文件系统的
扩展属性。

如果 `sync.local_conflict = "backup"` 已经保留本地内容，待安装 journal 还会
记录持久备份路径及其 SHA-256 指纹。只有备份和当前目标都与该指纹匹配时，恢复
流程才会替换仍然存在的目标。如果备份缺失、被修改或变成符号链接，恢复会停止，
不会冒险覆盖本地数据。

大文件的每个 Range 分片成功并完成 `fsync` 后，程序还会单独持久化 SQLite
`partial_download` 检查点。检查点记录远端 ETag、预期大小、目标、临时路径和
已经可靠写入的字节数。只有 HTTP 状态、`Content-Range`、总大小和实际接收字节
数都与请求范围完全一致，程序才会接受该分片；否则，分片会回滚到前一个可靠
offset。

后续进程只会在检查点元数据与同目录普通文件仍然匹配时续传。未形成检查点的
尾部字节会被截断；如果远端版本发生变化或状态已经陈旧，则从 byte 0 重新开始。
完整下载会先从 partial 状态转换到现有的待安装 journal，再执行原子替换。
如果 Delta 报告尚未下载完成的项目已被删除，或者完整刷新中不再包含该项目，
程序会在新 Delta 状态成功提交后删除对应检查点和临时文件。

Delta 查询期间，文本输出每完成一页打印一个点，最后汇总页数、扫描条目数、
去重后的变更数、文件、文件夹、删除项以及平均每页条目数。JSON 输出会逐页
产生包含 `pages`、`items` 和 `completed` 字段的 `delta_progress` 事件，
完成后再产生带有最终分类统计的 `delta_summary` 事件。Microsoft Graph
不会预先提供 Delta 条目总数，因此无法显示准确百分比。逐页明细仍可在 debug
日志中查看。`--quiet` 会隐藏控制台进度，但不会隐藏已配置的日志输出。

创建目录或下载文件前，同步流程会验证每个远端路径。程序会拒绝空路径、绝对
路径、`.`/`..` 路径段、NUL、控制字节和空路径段，还会根据目标文件系统的限制，
检查单个名称与完整路径的字节长度。

错误信息会指出远端路径、出现问题的具体组件、实际长度和支持上限。控制字节会
被转义，以确保诊断信息可以安全显示。非法名称会保存为 blocked item，其他文件
继续同步；通常需要在 OneDrive 中重命名这些项目。

下载进度会汇总所有并发传输。文本输出显示已完成/总文件数、总体字节百分比，
以及自动使用 B、KiB、MiB 或 GiB 的传输容量。交互式终端只原位刷新一条简短
进度；重定向文本和 JSON 输出每增加一个百分点产生一条事件。JSON 事件保留精确
字节数，并包含 `completed_files`、`file_count`、`downloaded_bytes`、
`total_bytes`、`percentage` 和 `completed` 字段。`--quiet` 会隐藏进度输出。

如果 Microsoft Graph 以 `410 Gone` 拒绝已保存的 Delta 游标，同步会自动改用
完整 Delta 查询重试。现有本地快照仍用于冲突检测。只有完整同步计划成功后，
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

Microsoft Graph 分页请求、下载重定向、文件下载和 Range 分片请求遇到 HTTP
408、429、502、503 或 504 响应时会重试。客户端会遵循数值形式的
`Retry-After` 响应头；如果响应头缺失或无效，则采用有上限的指数退避。

如果预认证下载 URL 返回 HTTP 401 或 403，客户端会从 Graph 获取新的 redirect，
并重试一次；Graph bearer token 不会发送给下载主机。Range 请求失败后，程序会
先将临时文件回滚到当前分片边界，再发起重试。重试次数有限；如果服务持续故障，
程序会明确失败，不会无限等待。

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

用户服务会运行长期驻留的 monitor。它会在启动时同步一次，通过 inotify 监听
本地 Drive 目录，按配置的周期轮询 Graph，并在意外失败后自动重启。
`systemctl --user stop` 会发送 `SIGTERM`，使服务安全退出。
