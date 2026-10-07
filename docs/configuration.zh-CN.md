# 配置

[English](configuration.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 配置文件与安全边界

默认优先读取 `~/.config/onedrive-cpp/config.toml`；文件不存在时使用内置默认值。
系统示例位于 `/etc/onedrive-cpp/onedrive-cpp.toml`。首次使用可执行：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

配置文件采用 TOML 格式，并且必须声明 `config_version = 2`。版本 1 的配置需要
将原有的 `sync.download_*` 键迁移到下文所示的 `transfer` 和 `download` 表。
程序会直接报告未知配置项和错误的值类型，不会静默忽略。

执行子命令前，客户端会将 `state.directory` 的权限收紧为仅所有者可访问的
`0700`，校验当前账号的 token 路径，并在该目录中持有独占的
`onedrive-cpp.lock`。如果另一个进程正在使用同一状态目录，新进程会立即失败。
对于当前用户拥有的现有私有状态文件，程序会将权限收紧为 `0600`；符号链接或
属于其他用户的文件会被拒绝。

同步时，`sync.data_directory` 和 `state.directory` 不能互相包含，不能把文件系统
根目录用作同步目录，并且所有已存在的路径组件都不能是符号链接。普通同步会在
访问 Graph 前执行创建、写入、`fsync`、删除探测；dry-run 仍保持不修改同步目录。
下载会按“实际传输量的 5%”与“256 MiB”两者中的较大值预留安全空间。并发工作
线程（worker）在传输前只预留各自尚未下载的字节，并在每个可靠 checkpoint 后
释放相应的承诺空间。如果可用容量已被其他活动下载预留，当前下载会等待。这样，
大批次任务可以依次推进，同时避免并发下载过度承诺磁盘空间。

`sync.drive_id` 指定要访问的远端 OneDrive Drive。默认值 `me` 表示当前登录
账号的默认 OneDrive，程序通过 Microsoft Graph 路径
`/me/drive/root/children` 列出根目录。如果需要访问该账号有权使用的其他
OneDrive 或 SharePoint 文档库，请将其设置为实际的 Drive ID；程序将改用
`/drives/<drive_id>/root/children`。

## 配置参考示例

以下示例展示全部主要配置组：

```toml
[sync]
# 当前账号的默认 OneDrive
data_directory = "/home/USER/OneDrive"
drive_id = "me"
permissions = "private"
local_conflict = "block"
# 可选；已挂载的祖先目录，Monitor 每轮同步前也会检查
# data_mount_point = "/mnt/data"
# 可选；相对路径以本 TOML 文件所在目录为基准
# sync_list = "sync_list"
sync_root_files = false
# 普通文件 .nosync 会排除其所在目录子树
nosync_enabled = true
# "include" 或 "exclude"
dotfiles = "include"
# 0 表示不限制
maximum_file_size_bytes = 0
mode = "bidirectional"
delete_policy = "propagate"
maximum_remote_deletions = 1000

# 指定其他 OneDrive 或 SharePoint 文档库
# drive_id = "b!YOUR_DRIVE_ID"

# 可选；同时作用于认证、Graph API 和文件下载
# [proxy]
# url = "socks5h://127.0.0.1:1080"
# no_proxy = ["localhost", "127.0.0.1", ".internal.example.com"]
# username = "proxy-user"
# password_file = "/run/secrets/onedrive-proxy-password"
# auth = "auto"
# ca_file = "/etc/ssl/certs/company-proxy-ca.pem"

[transfer]
order = "default"
connect_timeout_seconds = 30
operation_timeout_seconds = 3600
stall_timeout_seconds = 60
stall_minimum_bytes_per_second = 1
http_version = "auto"
ip_version = "auto"

[download]
concurrency = 4
maximum_retries = 4
chunk_threshold_bytes = 8388608
checkpoint_interval_bytes = 1048576
maximum_rate_bytes_per_second = 0
maximum_total_rate_bytes_per_second = 0
validation = "strict"

[upload]
# 独立文件可以并发上传；单个 upload session 内的分片仍保持顺序。
concurrency = 1
# 不超过 250 MB（250,000,000 字节）的文件使用简单上传；更大的文件使用
# upload session，下面的配置指定每个分片的大小。
chunk_size_bytes = 10485760
maximum_rate_bytes_per_second = 0
maximum_total_rate_bytes_per_second = 0

[monitor]
websocket_enabled = true
websocket_request_timeout_seconds = 60
websocket_connect_timeout_seconds = 10
websocket_renewal_lead_seconds = 120
websocket_initial_backoff_seconds = 1
websocket_maximum_backoff_seconds = 300
poll_interval_seconds = 300
settle_delay_milliseconds = 1000
```

## 存储与挂载安全

如果 `sync.data_directory` 位于可移动存储、网络文件系统或其他可能掉线的挂载
盘，请设置 `sync.data_mount_point`。该路径必须是当前实际挂载的目录，并且必须
是 `sync.data_directory` 的祖先目录。客户端会在启动时检查该路径，并在每轮
完整同步或单文件下载前再次检查。

如果 Monitor 运行期间挂载盘消失，下一轮同步会在本地扫描、恢复 pending 操作、
下载、上传、远端删除或更新 Delta 游标之前停止。Monitor 会在后续调度中重试，
因此，只要同一路径重新挂载，程序就能恢复正常工作。

此选项默认关闭，客户端不会根据 `sync.data_directory` 猜测挂载点。例如
`/mnt/data/OneDrive` 是同步目录、`/mnt/data` 是实际挂载点时，配置为：

```toml
[sync]
data_directory = "/mnt/data/OneDrive"
data_mount_point = "/mnt/data"
```

## 大批量删除保护

`sync.maximum_remote_deletions` 限制单次本地删除计划最多可以从 OneDrive 删除
多少个已跟踪项目，默认值为 `1000`。设为 `0` 后，除非显式强制执行，否则任何
远端删除都会被阻止。如果已跟踪目录消失，计数会包含目录本身及其所有已跟踪
后代，即使 Graph 只需对父目录发出一次 DELETE。客户端会在上传侧执行移动、
创建目录、上传或删除之前检查整个批次，并在实际删除前再次检查。

超过限制时，普通 sync 和 monitor 都会在发出远端 DELETE 前停止；monitor 绝不
会自动绕过这项保护。`sync --dry-run` 会报告 Graph 删除操作数、受影响的快照
数量、限制值，以及普通运行是否会被阻止。确认本地文件系统状态无误，并确定删除
符合预期后，可以执行一次性强制操作：

```bash
onedrive-cpp sync --force-large-delete
```

该开关只作用于本次命令，不能写入配置文件。pending-delete 崩溃恢复也使用同一
限制，因此重启进程不能绕过保护。

## Monitor 调度与通知

`monitor` 启动后，先执行一轮完整同步并获取 Microsoft Graph Socket.IO
channel，然后进入休眠。收到远端 WebSocket 通知、inotify 报告完整的本地变化，
或 Graph 轮询周期到期时，它会被唤醒。本地事件突发会按照
`monitor.settle_delay_milliseconds` 合并。即使没有本地活动，当通知 channel
获取、续期或投递失败时，`monitor.poll_interval_seconds` 仍会限制发现远端变化
的最长延迟。

WebSocket 通知只负责唤醒。每次通知以及重连后的 catch-up 仍通过权威 Delta
查询完成收敛。程序会递归监听新建或移入的目录树；如果 inotify 队列溢出，则
重建全部 watch，并安排一次完整同步。
`SIGINT` 和 `SIGTERM` 会唤醒阻塞等待，并在当前同步结束后安全退出。
设置 `monitor.websocket_enabled = false` 可禁用 Graph Socket.IO/WSS 通知；
本地 inotify 事件和 Graph 定时轮询仍保持启用。request/connect timeout 分别限制
channel 获取和 WSS 握手时间，renewal lead 控制提前续期时间，initial/maximum
backoff 限制指数退避范围。Engine.IO heartbeat 时间由服务器协商，因此不提供
本地配置。
Monitor 调度器采用显式的单线程运行时状态机，状态包括 starting、idle、本地
事件 settling、synchronizing 和 stopped。本地事件突发会重置 settle deadline；
队列溢出会提升待处理原因的优先级；尚未完成的本地 settle 优先于已经到期的
Graph poll。系统 I/O 和 `SyncEngine` 位于纯状态 reducer 之外，因此调度器只
约束事件顺序，不会重复实现同步策略。

## 选择与过滤

`sync.sync_list` 用于启用客户端选择性同步。它指向一个独立的 UTF-8 规则文件；
相对路径以 TOML 配置文件所在目录为基准解析。未配置时，所有远端项目都可以参与
同步；配置后规则文件必须可读，空规则文件表示不选择任何远端项目。

规则文件默认排除所有内容，并支持：

- 空行和以 `#` 开头的注释行；
- `/Documents/`、`Pictures/*.jpg` 等包含规则；
- 以 `!` 或 `-` 开头的排除规则；
- 前导 `/` 将规则限定在 Drive 根目录；
- 尾随 `/` 将规则限定为目录及其后代；
- `*` 匹配单个路径段内的字符，完整路径段 `**` 递归匹配任意深度。

例如：

```text
# 包含 Documents，但排除私密内容和临时文件
/Documents/
!/Documents/Private/*
!/Documents/**/*.tmp

# 在任意目录深度包含匹配的图片
Pictures/*.jpg
```

排除规则优先于包含规则。程序会保留创建所选文件所需的父目录。没有前导 `/`
的规则可以在任意深度匹配，因此作用范围更广。反斜杠、空路径组件、`.`、`..`
以及嵌入其他字符中的 `**` 会被拒绝。

Microsoft Graph 返回 Delta 元数据后，客户端才会应用过滤规则。过滤可以减少
本地文件数量和内容传输量，但不属于 Graph 服务端过滤。程序会在同一个 SQLite
事务中提交有效规则摘要和 Delta 游标。增加、修改、删除规则或调整规则顺序后，
下一次同步会自动获取完整的远端状态。

选择范围变化不属于远端删除，因此，本地已有但后来被排除的文件会明确保留。
如果已跟踪文件从包含路径移到排除路径，schema v16 SQLite 状态会根据该保留对象
的 device/inode 身份抑制上传。即使之后在本地修改同一对象，程序也不会错误地从
旧路径上传。如果对象消失，或被具有不同 filesystem identity 的对象替换，下一次
上传扫描会清理失效的抑制记录。远端项目重新移入选择范围后，程序会下载其当前
路径，但不会解除对旧保留副本的保护。用户手动执行的
`download REMOTE_PATH` 不受 `sync.sync_list` 限制。

配置 `sync.sync_list` 后，设置 `sync.sync_root_files = true` 会自动包含直接位于
Drive 根目录中的普通文件。根目录下的目录及其后代仍然必须由包含规则选中，
`!/root-secret.txt` 之类的排除规则优先于自动包含。默认值为 `false`；未配置
`sync.sync_list` 时，普通同步本来就会包含所有文件，因此该设置没有效果。修改
该值会改变选择性同步摘要，下次同步会先执行完整远端状态查询，再提交新的选择。

同一个过滤器也用于下载、上传、本地移动发现和远端删除规划：

- `sync.nosync_enabled = true`（默认值）把普通文件 `.nosync` 视为本地标记，
  排除标记所在目录及其完整子树；标记本身永远不会参与同步，名为 `.nosync`
  的符号链接不算标记。
- `sync.dotfiles = "include"` 保持 Unix dotfile 可同步；设为 `"exclude"` 后，
  路径中任意以 `.` 开头且不止一个字符的组件都会使该路径被排除。
- `sync.maximum_file_size_bytes = 0` 表示不限制大小。正值会在两个同步方向上
  排除严格大于该字节数的普通文件；大小恰好等于限制值的文件仍可同步。

被过滤的文件如果在本地不存在，不会被视为远端删除。修改这些策略，或增加、
删除 `.nosync` 标记，会更新过滤摘要并触发完整 Delta 查询。程序会保留后来被
排除的现有本地文件，并根据 filesystem identity 抑制上传。文件重新符合条件时，
仍会执行正常的本地冲突保护。显式执行的 `download REMOTE_PATH` 命令不受这些
自动同步过滤器限制。

## 冲突处理

`sync.local_conflict` 决定如何处理同时发生的本地和远端文件变化。默认值
`"block"` 保持原有行为：普通同步会将项目记录为 `local_modification`，显式
单文件下载则会在传输开始前停止。设为 `"backup"` 后，程序会先将稳定的本地
内容复制为同目录下的持久备份，例如
`report.safeBackup-20261004T051000Z-0001.pdf`，然后再原子安装远端权威版本。
备份是独立副本而不是硬链接，会保留本地权限位，并会被排除在上传之外。
如果本地内容与已下载内容的 SHA-256 指纹相同，程序不会创建备份，也不会替换
原 inode，而是直接采用现有文件。创建备份需要额外占用约等于本地文件大小的
磁盘空间。如果备份失败，程序会安全停止，不会替换目标。该策略同时用于普通同步、
`download REMOTE_PATH`，以及恢复上传时发现的并发远端变化。上传恢复只会丢弃
已经过期的上传快照和 journal，再使用同一策略与远端 delta 对账。对于无法创建
安全普通文件副本的目录冲突和远端删除冲突，程序仍会阻止操作。

对于已跟踪文件，状态数据库会同时保存 Graph eTag 和 cTag。本地快照未变化时，
如果 delta 只改变 eTag，而非空 cTag 保持一致，程序只刷新远端元数据，不会重新
下载文件。cTag 缺失或发生变化时会保守地退回 eTag 判定并下载远端内容。目录判定
不依赖 cTag，因为 SharePoint 和 OneDrive for Business 可能不返回目录 cTag，或
不能一致地反映后代变化。

## 同步模式与删除策略

`sync.mode` 默认为 `bidirectional`，会下载远端变化并上传符合相同 sync-list
规则的本地变化。`upload_only` 仍会获取并保存 Graph Delta 基线，但不会下载、
移动或删除本地内容；本地新增和修改仍使用已有冲突检查和持久上传 journal。
`download_only` 会下载远端变化，但不扫描本地变化进行上传，也不会恢复
pending upload、远端移动或远端删除操作。

在 `download_only` 模式下，如果设置
`sync.delete_policy = "propagate"`，远端源项目删除后，程序会安全删除未变化的
已跟踪本地项目。遇到本地修改、类型异常或非空目录时，程序会阻止删除，并通过
现有的 blocked-item journal 重试。`preserve` 会保留本地实体，但会消费远端
tombstone、移除对应的跟踪状态，并清理旧的 blocked deletion，避免完整 Delta
刷新反复规划同一删除。应用最新远端 Delta 之前，程序仍会恢复 pending download。

设置 `sync.delete_policy = "preserve"` 后，本地项目缺失不会删除远端对应项目；
当 `upload_only` 未显式设置删除策略时，这也是安全默认值。双向同步默认使用
`propagate`。旧 `sync.upload` 布尔值继续兼容：`true` 映射为
`bidirectional`，`false` 映射为 `download_only`，但不能与 `sync.mode`
同时配置。

新文件使用“冲突即失败”创建，已跟踪文件使用保存的 eTag 作为 `If-Match`
前置条件。符号链接、safeBackup 和传输临时名称、被阻止的远端路径以及类型冲突
都不会上传。每次传输使用稳定的私有快照和持久 SQLite pending-upload journal；
恢复时会下载已经出现的远端文件并比较 SHA-256，匹配后才提交状态。符合
selective sync 规则且尚未跟踪的本地目录会在其文件上传前按
父目录优先顺序创建到远端。目录创建使用“冲突即失败”和相同的持久 journal：
Graph 明确返回冲突时会移除 journal 并阻止操作；发生结果不明确的中断后，恢复
流程会重试请求，并且只有同一路径的远端项目确实是目录时才采用它。
## 上传、移动与远端修改

不超过 250 MB 的文件使用简单上传；更大的文件使用 Microsoft Graph upload
session 连续分片上传。只有 Graph 通过 `nextExpectedRanges` 精确确认后，程序
才会推进上传偏移。
默认分片大小为 10 MiB；非末尾分片必须是 320 KiB 的整数倍，并低于 Graph 的
60 MiB 单请求上限。预授权 upload session URL 不会携带 Graph Authorization
header，也不会写入日志。pending-upload journal 会持久保存 session URL、过期时间，以及 Graph 每次确认
的偏移。进程重启后，程序会在不携带 Authorization header 的情况下查询 session。
如果 Graph 进度领先于本地最后一个 checkpoint，程序会先持久化远端进度，再从
该位置续传，不会重发已确认的分片。session 过期或返回 HTTP 404/410 时，程序会
安全创建新 session；如果服务端偏移落后于可靠 checkpoint，程序会停止，避免
重复发送数据。旧配置中的 `upload = false` 会映射为
`mode = "download_only"`。

如果收到 OneDrive 配额响应，或本地上传发生读取、权限、快照空间或 I/O 错误，
程序会在同一个 pending-upload journal 中持久记录可操作的原因和尝试次数。单个
项目失败不会阻止其他上传。每轮同步会对已记录的失败重试一次；恢复成功后清除
journal，重复失败则继续通过警告和 blocked 汇总显示。

已跟踪本地项目消失后，程序会使用保存的 eTag 作为 `If-Match` 前置条件删除远端
项目。目录删除按父目录优先处理，并同时清理其已跟踪后代。独立的 SQLite journal
使 Graph 已完成但进程尚未提交状态的删除可以在重启后恢复，因此 HTTP 404 被视为
幂等成功；409/412 则停止操作且不丢弃已跟踪状态。dry-run 和当前 sync-list 之外
的路径不会发起删除。
已跟踪的本地文件和目录还会持久保存文件系统 device/inode identity。下载和上传
会立即记录它，普通非 dry-run 上传扫描则会回填旧 snapshot。该稳定身份是后续
安全识别本地移动的基础，避免依赖有歧义的大小和时间戳匹配。识别出的移动使用
保存的 eTag 执行条件 Graph PATCH，并写入独立 SQLite journal。文件在移动同时被
修改时，会在移动提交后继续上传内容；目录移动则原子重映射已跟踪后代。重启恢复
只有在远端 ID 和本地 filesystem identity 都匹配时才采用目标。如果目标父目录
是在本地新建的，同步会先按从浅到深的顺序创建所有缺失远端父目录，再执行条件
移动。父目录创建和移动继续使用各自独立的持久 journal。

当 Delta 项目的远端 ID 保持不变但路径发生变化时，程序会在本地安全执行重命名或
移动，并且绝不覆盖已经存在的目标。Graph 只报告被移动目录本身时，程序也会重映射
SQLite 中所有已跟踪后代的路径。远端内容指纹匹配的文件会直接复用；没有指纹时使用
大小和权威内容修改时间判断。若文件在移动同时发生内容变化，则先移动再下载新内容。
当名称交换或其他依赖环出现时，程序会把一个 breaker 原子移动到同步根目录内的
私有隐藏 staging 路径，按依赖顺序完成其余移动，再安装被暂存的项目。
本地已修改的源文件、符号链接、类型冲突和已占用目标会作为可重试 blocked item
保留。文件系统移动及源、目标父目录会在推进 Delta 游标前刷盘；中断后的重试可以
安全认领已经移动完成的目标。
跨文件系统边界的移动会保留为 `cross_device_move` blocked item；程序不会跨挂载
点复制后删除数据。
执行原子移动前，schema v16 SQLite 状态会记录源路径、目标路径、可选 staging
路径及源对象的 device/inode 身份。移动后的 item 与 Delta 游标提交会在同一事务
中删除 journal。中断恢复会在原路径、staging 路径和最终目标中查找完全匹配的
filesystem identity；`reset-state` 保留这些记录，`--clear-all` 会删除它们。

## 云端入口与代理

`graph.endpoint` 用于选择 Microsoft Graph 云端点，默认使用全球服务，并不与
某个具体 SharePoint 主机名绑定。访问由世纪互联运营的 Microsoft 365 中国区
SharePoint 文档库时，应同时配置 Graph 和认证云端点：

```toml
[auth]
endpoint = "https://login.chinacloudapi.cn"

[graph]
endpoint = "https://microsoftgraph.chinacloudapi.cn/v1.0"
```

可选的 `proxy.url` 会让认证、Microsoft Graph 和文件下载请求统一通过同一个代理。
支持的 URL scheme 为 `http`、`https`、`socks4`、`socks4a`、`socks5` 和
`socks5h`。如果需要由代理服务器解析目标域名，应使用 `socks5h` 而不是
`socks5`：

```toml
[proxy]
url = "https://proxy.example.com:8443"
no_proxy = ["localhost", "127.0.0.1", ".internal.example.com"]
username = "proxy-user"
password_file = "/run/secrets/onedrive-proxy-password"
auth = "auto"
ca_file = "/etc/ssl/certs/company-proxy-ca.pem"

# 或通过 SOCKS5 进行远端 DNS 解析：
# url = "socks5h://127.0.0.1:1080"
```

完整的代理配置默认行为如下：

- `proxy.url`、`proxy.no_proxy`、`proxy.username`、`proxy.password_file` 和
  `proxy.ca_file` 默认均未设置；
- `proxy.auth` 默认为 `auto`；
- `proxy.url` 未设置时，程序不会主动指定代理，但 libcurl 仍可能读取环境中的
  `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY` 及其小写形式；
- `proxy.no_proxy` 未设置时，libcurl 使用环境中的 `NO_PROXY`/`no_proxy`；
- `proxy.ca_file` 未设置时，HTTPS 代理使用系统 CA 信任库。

若要保证完全直连，应省略 `[proxy]` 配置段，并清除所有代理环境变量。若要让
配置的代理忽略环境中的 `NO_PROXY` 并代理所有目标，应设置 `no_proxy = []`。

`proxy.no_proxy` 是主机、域名后缀、IP 地址或其他 libcurl 免代理模式的数组。
未配置时继续使用 libcurl 的 `NO_PROXY`/`no_proxy` 环境变量；显式配置空数组会
覆盖环境变量，让所有目标都通过代理。

`proxy.username` 用于启用代理凭据。对应密码应存放在 `proxy.password_file`
指向的文件中，不应写入 TOML 或代理 URL。相对密码文件路径以 TOML 文件所在
目录为基准解析。密码文件必须是不经过符号链接访问的普通文件，不能授予 group
或 other 任何权限，且不能超过 64 KiB。程序会移除末尾的一个 LF 或 CRLF；
空密码、内含 NUL 字节或缺少用户名的密码文件会被拒绝。

`proxy.auth` 控制 HTTP/HTTPS 代理认证，支持 `auto`、`basic`、`digest`、
`ntlm` 和 `negotiate`，默认值为 `auto`。SOCKS5 使用自身的用户名/密码认证。
实际可用的认证机制取决于系统安装的 libcurl 构建。

`proxy.ca_file` 为 HTTPS 代理增加 CA 文件，其他代理 scheme 配置该字段会被
拒绝。相对路径同样以 TOML 文件所在目录为基准。代理证书及主机名校验始终开启，
配置中不能将其禁用。不要把代理凭据写入 `proxy.url`，因为配置内容或错误诊断
可能暴露 URL。

## 传输行为与限制

`download.concurrency` 控制可以同时下载的文件数量，默认值为 `4`，允许范围为
`1` 到 `16`。如果多个下载指向同一个规范化本地路径，它们始终会串行执行；这也
包括仅有 ASCII 字母大小写差异的常见情况。目标无关的下载仍可并发执行。

`transfer.order` 控制文件传输进入 worker 队列的顺序，支持 `default`、
`size_asc`、`size_dsc`、`name_asc` 和 `name_dsc`。默认保留同步计划顺序，
排序键相同时也保持原顺序。并发执行时，它控制任务开始顺序而非完成顺序。
该设置作用于下载队列；上传依赖继续使用父目录优先的操作顺序。

`download.chunk_threshold_bytes` 设置大文件阈值（字节）。超过该值的文件会通过
HTTP 字节范围请求顺序分片下载，并以该值作为单个分片的最大大小。默认值为
`8388608`（8 MiB），且必须大于零。等于或小于阈值的文件仍使用单次请求。
程序会在写入响应正文前验证 Range 响应元数据。单请求下载和宽松模式下载也会
先拒绝非成功 HTTP 响应的正文，防止它进入临时文件或 durable checkpoint。
大型传输会在 Graph 内容请求中通过 `If-Match` 提交预期的远端 eTag。如果远端
版本发生变化，Graph 会在签发下载 URL 前拒绝请求。

大型传输期间，程序会定期可靠写盘并记录 checkpoint。请求中断后，会从最后一个
安全落盘的偏移量继续，而不是重新下载整个分片。用户正常取消时，程序也会在停止
前可靠写盘，并记录已经通过 Range 响应验证的字节。

`download.checkpoint_interval_bytes` 控制每新增多少下载字节就可靠写盘并记录
可续传进度。默认值为 `1048576`（1 MiB），且必须大于零。更小的值可以减少
中断后的重复下载量，但会增加同步写盘和数据库更新开销。

`download.maximum_retries` 控制文件内容请求遇到临时 HTTP 或传输错误后的最大
重试次数。默认值为 `4`；设为 `0` 可禁用文件内容重试。它独立于
`graph.throttle.maximum_retries`，后者仍控制 Microsoft Graph API 请求重试。
下载重试继续使用 Graph 的退避延迟配置。

`download.maximum_rate_bytes_per_second` 限制单个文件内容请求的接收速率，默认
值为 `0`，表示不限制。`download.maximum_total_rate_bytes_per_second` 限制所有
并发文件下载共享的总接收速率，默认值同样为 `0`。两者都非零时，每个请求受
单请求上限约束，同时所有活动请求共同受总上限约束。总限速使用公平且可取消的
令牌桶，突发量最多为 64 KiB。限速等待时间会计入
`transfer.operation_timeout_seconds`，也可能影响 libcurl 的停滞检测，因此非常
低的限速可能需要同时提高操作超时或停滞超时。

`upload.maximum_rate_bytes_per_second` 限制单个上传请求的发送速率，默认值为
`0`。`upload.concurrency` 控制可同时上传的独立文件数量，默认值为 `1`，允许
范围为 `1` 到 `16`。单个 Microsoft Graph upload session 内的分片始终顺序
上传；目录会在依赖它的文件进入 worker 队列前创建完成。
`upload.maximum_total_rate_bytes_per_second` 提供所有活动上传共享的总发送上限，
默认值为 `0`，并复用与总下载限速相同的公平、可取消令牌桶。
`upload.chunk_size_bytes` 默认为 10 MiB，必须是 320 KiB 的正整数倍且小于
60 MiB。

`transfer.ip_version` 接受 `"auto"`、`"4"` 或 `"6"`，默认值为 `"auto"`。
强制指定地址族可绕过异常的 IPv6 或 IPv4 路由，但下载主机在该地址族下没有
可用地址时，请求会明确失败。

预认证下载 URL 最多允许继续跳转五次，且所有目标都必须使用 HTTPS；Graph
Authorization 不会附加到这些 CDN 请求。每个下载工作线程都会安全复用经过
完整重置的 libcurl easy handle，使分片、
重试和后续文件能够复用 DNS、TCP、TLS 与 HTTP/2 连接状态，同时不会在不同
请求之间遗留 header、正文或回调。trace 日志会以微秒为单位记录协商的 HTTP
版本、新建连接数量以及 DNS、TCP、TLS、服务端等待、正文传输和总耗时，但
不会记录请求 URL、header 或正文。

下载进度会聚合所有活动文件，并显示当前平滑传输速率和预计剩余时间；最终进度
还会显示下载总耗时。JSON 进度事件通过 `bytes_per_second`、
`estimated_seconds_remaining` 和 `elapsed_milliseconds` 提供相同指标。

`download.validation` 默认为 `"strict"`，要求下载大小以及 Graph 提供的内容哈希
与远端元数据一致。部分 SharePoint、Azure Information Protection（AIP）和
HEIC 文件实际下载的字节可能与 Graph 元数据不同；`"relaxed"` 会接受这类文件，
但会禁用断点续传、分块下载和远端大小/哈希校验。HTTP 成功状态、可靠写盘、
原子安装以及用于崩溃恢复的本地 SHA-256 指纹仍会强制执行。磁盘空间会按照
实际传输进度动态预留，而不是信任 Graph 的大小元数据；无法安全扩充预留时会
中止传输。由于 Graph 无法在下载前可靠识别 AIP 文件，宽松模式会作用于所有
下载，并会降低完整性保证。

## 权限与存储布局

`permissions` 默认为 `"private"`。新同步文件以 `0600` 权限创建，同步根目录和
新目录则设为 `0700`，防止本机其他用户读取同步内容。只有确实需要通过 Unix 组
权限共享同步目录时，才应将其设为 `"umask"`，使权限遵循进程 umask。打包的
systemd 用户服务还会使用 `UMask=0077`，提供纵深防御。

`sync.data_directory` 是同步数据的公共根目录。实际 Drive 内容会使用与 state 相同的
稳定 ID 和友好名称组件进行隔离：

```text
<sync.data_directory>/accounts/<显示名称>--<用户-ID-哈希>/
  drives/<Drive-名称>--<Drive-ID-哈希>/
    <同步的 OneDrive 内容>
```

因此，同一个配置根目录可以同时容纳多个 Microsoft 用户和多个 Drive，而不会
发生路径冲突。旧的平面 `<sync.data_directory>` 布局中的文件不会自动移动，
仍会保持原样。

状态按稳定的 Microsoft 用户 ID 和真实 Drive ID 隔离，同时保留友好的目录名：

```text
<state.directory>/accounts/<显示名称>--<用户-ID-哈希>/
  account.json
  avatar.<图片扩展名>
  refresh_token
  drives/<Drive-名称>--<Drive-ID-哈希>/
    drive.json
    items.sqlite3
```

账号和 Drive 目录包含稳定 ID 哈希，因此显示名称改变时不会创建第二套状态目录。
Drive 数据库除远端 ID、ETag 和本地路径外，还会保存并校验用户 ID、显示名称、
真实 Drive ID、Drive 名称、头像 MIME 类型和头像二进制内容。SQLite 使用 WAL
模式。数据库中的 `drive_mapping` 表会记录配置选择器与解析结果，例如
`me` 到 Microsoft 真实 Drive ID 的映射。

旧的平面 `<state.directory>/items.sqlite3` 和
`<state.directory>/refresh_token` 布局不会自动迁移。升级后需要重新运行
`onedrive-cpp auth` 初始化账号目录，并重新建立同步状态。
