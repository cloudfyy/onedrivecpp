# 配置

[English](configuration.md) | 简体中文

默认优先读取 `~/.config/onedrive-cpp/config.toml`；文件不存在时使用内置默认值。
系统示例位于 `/etc/onedrive-cpp/onedrive-cpp.toml`。首次使用可执行：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

配置文件使用 TOML，并且必须声明 `config_version = 2`。版本 1 配置必须把原来的
`sync.download_*` 键迁移到下方的 `transfer` 和 `download` 表。未知配置项和错误
的值类型会直接报错，不会被静默忽略。

执行子命令前，客户端会把 `state.directory` 收紧为仅所有者可访问的 `0700`，
校验当前账号 token 路径，并在该目录持有独占的 `onedrive-cpp.lock`。第二个
使用同一状态目录的进程会立即失败。当前用户拥有的既有私有状态文件会收紧为
`0600`；符号链接或其他用户拥有的文件会被拒绝。

同步时，`sync.directory` 和 `state.directory` 不能互相包含，不能把文件系统
根目录用作同步目录，并且所有已存在的路径组件都不能是符号链接。普通同步会在
访问 Graph 前执行创建、写入、`fsync`、删除探测；dry-run 仍保持不修改同步目录。
下载会保留实际传输量 5% 或 256 MiB 中的较大值作为安全余量。并发 worker 在
传输前只预留各自尚未下载的字节，每个可靠 checkpoint 后释放对应承诺空间；当
可用容量已被其他活动下载预留时会等待。这样大批次可以顺序推进，同时不会让
并发下载过量承诺磁盘空间。

`sync.drive_id` 指定要访问的远端 OneDrive Drive。默认值 `me` 表示当前登录账号的
默认 OneDrive，程序使用 Microsoft Graph 路径 `/me/drive/root/children`
列出其根目录。若要访问账号有权使用的其他 OneDrive 或 SharePoint 文档库，
可将其设置为实际的 Drive ID；程序将改用
`/drives/<drive_id>/root/children`。例如：

```toml
[sync]
# 当前账号的默认 OneDrive
drive_id = "me"
permissions = "private"
local_conflict = "block"
# 可选；相对路径以本 TOML 文件所在目录为基准
# sync_list = "sync_list"
sync_root_files = false
upload = true
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
poll_interval_seconds = 300
settle_delay_milliseconds = 1000
```

`sync.maximum_remote_deletions` 限制一次本地删除计划最多可从 OneDrive 删除
多少个已跟踪项目，默认值为 `1000`；设为 `0` 时，除非显式强制，否则任何远端
删除都会被阻止。已跟踪目录消失时，计数包含目录自身及其所有已跟踪后代，即使
Graph 只需要对父目录发出一次 DELETE。客户端会在上传侧移动、建目录、上传或
删除发生前检查完整批次，并在真正执行删除前再次检查。

超过限制时，普通 sync 和 monitor 都会在发出远端 DELETE 前停止；monitor 永不
自动绕过保护。`sync --dry-run` 会报告 Graph 删除操作数、受影响快照数、限制值
以及普通运行是否会被阻止。确认本地文件系统状态且删除确属预期后，可执行一次性
强制：

```bash
onedrive-cpp sync --force-large-delete
```

该开关只作用于本次命令，不能写入配置文件。pending-delete 崩溃恢复也使用同一
限制，因此重启进程不能绕过保护。

`monitor` 启动时先执行一轮完整同步，随后休眠，直到 inotify 报告已完成的本地
变化，或者 Graph 轮询周期到期。本地事件突发会按
`monitor.settle_delay_milliseconds` 合并；即使没有本地活动，
`monitor.poll_interval_seconds` 也限制远端变化的最长发现延迟。新建或移入的
目录树会被递归监听；inotify 队列溢出时会重建全部 watch 并安排完整同步。
`SIGINT` 和 `SIGTERM` 会唤醒阻塞等待，并在当前同步结束后安全退出。
Monitor 调度器使用显式的单线程运行时状态机，状态包括 starting、idle、本地事件
settling、synchronizing 和 stopped。本地事件突发会重置 settle deadline，队列
溢出会升级待处理原因，并且待完成的本地 settle 优先于已到期的 Graph poll。
系统 I/O 和 `SyncEngine` 保持在纯状态 reducer 之外，因此调度器只约束事件顺序，
不会复制同步策略。

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

过滤发生在 Microsoft Graph 返回 Delta 元数据之后；它可以减少本地文件和内容
传输，但不是 Graph 服务端过滤。有效规则的摘要会与 Delta 游标在同一个 SQLite
事务中提交。增加、修改、删除或重新排序规则后，下次同步会自动获取完整远端
状态。选择范围变化不是远端删除记录，因此已经存在于本地但后来被排除的文件会
被明确保留。当已跟踪文件从包含路径移动到排除路径时，schema v16 SQLite 状态会
用该保留对象的 device/inode 身份建立上传抑制；即使同一对象随后被本地修改，也
不会从旧路径错误上传。对象消失或被不同 filesystem identity 替换后，下次上传
扫描会清理失效抑制。远端项目重新移入选择范围时会下载当前路径，但不会解除旧保留
副本的保护。用户执行的 `download REMOTE_PATH` 不受 `sync.sync_list` 限制。

配置 `sync.sync_list` 后，设置 `sync.sync_root_files = true` 会自动包含直接位于
Drive 根目录中的普通文件。根目录下的目录及其后代仍然必须由包含规则选中，
`!/root-secret.txt` 之类的排除规则优先于自动包含。默认值为 `false`；未配置
`sync.sync_list` 时，普通同步本来就会包含所有文件，因此该设置没有效果。修改
该值会改变选择性同步摘要，下次同步会先执行完整远端状态查询，再提交新的选择。

`sync.local_conflict` 控制同时发生的本地和远端文件变化。默认值
`"block"` 保持原有行为：普通同步把项目记录为 `local_modification`，显式单文件
下载则在开始传输前停止。设置为 `"backup"` 后，程序会先把稳定的本地内容复制到
同目录的持久备份，例如
`report.safeBackup-20261004T051000Z-0001.pdf`，然后再原子安装远端权威版本。
备份是独立副本而不是硬链接，会保留本地权限位，并会被排除在上传之外。
如果本地内容与已下载内容的 SHA-256 指纹相同，程序不会创建备份，也不会替换原
inode，而是直接采用现有文件。创建备份需要额外占用约等于本地文件大小的磁盘
空间；失败时程序会安全停止，不会替换目标。该策略同时作用于普通同步、
`download REMOTE_PATH`，以及恢复上传时发现的并发远端变化。上传恢复只会丢弃
已经过期的上传快照和 journal，然后通过同一策略对账远端 delta。无法创建安全
普通文件副本的目录冲突和远端删除冲突仍会被阻止。

对于已跟踪文件，状态数据库会同时保存 Graph eTag 和 cTag。本地快照未变化时，
如果 delta 只改变 eTag，而非空 cTag 保持一致，程序只刷新远端元数据，不会重新
下载文件。cTag 缺失或发生变化时会保守地退回 eTag 判定并下载远端内容。目录判定
不依赖 cTag，因为 SharePoint 和 OneDrive for Business 可能不返回目录 cTag，或
不能一致地反映后代变化。

`sync.upload` 默认为 `true`。应用远端变化后，普通同步会上传符合相同 sync-list
规则的本地新增和修改普通文件。新文件使用“冲突即失败”创建，已跟踪文件使用保存
的 eTag 作为 `If-Match` 前置条件。符号链接、safeBackup 和传输临时名称、被阻止
的远端路径以及类型冲突都不会上传。每次传输使用稳定的私有快照和持久 SQLite
pending-upload journal；恢复时会下载已经出现的远端文件并比较 SHA-256，匹配后
才提交状态。符合 selective sync 规则且尚未跟踪的本地目录会在其文件上传前按
父目录优先顺序创建到远端。目录创建使用“冲突即失败”和相同的持久 journal：
Graph 明确返回冲突时会移除 journal 并阻止操作；发生结果不明确的中断后，恢复
流程会重试请求，并且只有同一路径的远端项目确实是目录时才采用它。
250 MB 以内使用简单上传，更大的文件使用 Microsoft Graph upload
session 连续分片，并且只推进到 Graph 通过 `nextExpectedRanges` 精确确认的偏移。
默认分片大小为 10 MiB；非末尾分片必须是 320 KiB 的整数倍，并低于 Graph 的
60 MiB 单请求上限。预授权 upload session URL 不会携带 Graph Authorization
header，也不会写入日志。pending-upload journal 会持久保存 session URL、过期时间
以及 Graph 每次确认的偏移。进程重启后，程序会在不携带 Authorization header 的
情况下查询 session；如果 Graph 进度领先于本地最后一个 checkpoint，则先持久化
远端进度，再从该位置续传，不会重发已确认分片。session 过期或返回 HTTP 404/410
时会安全创建新 session；服务端偏移落后于可靠 checkpoint 时会停止，避免重复发送
数据。设置 `upload = false` 可保持仅下载行为。

OneDrive 配额响应，以及本地上传读取、权限、快照空间或 I/O 失败，会在同一个
pending-upload journal 中持久记录可操作原因和尝试次数。单个失败项目不会阻止
其他上传。每轮同步会对已记录失败重试一次；恢复成功后清除 journal，重复失败则
继续通过警告和 blocked 汇总显示。

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

`download.concurrency` 控制可同时下载的文件数量，默认值为 `4`，允许范围为
`1` 到 `16`。指向同一规范化本地路径的下载始终会串行执行，包括常见的仅
ASCII 大小写不同的路径；无关目标仍可并发下载。

`transfer.order` 控制文件传输进入 worker 队列的顺序。支持 `default`、
`size_asc`、`size_dsc`、`name_asc` 和 `name_dsc`。默认保留同步计划顺序，
排序键相同时也保持原顺序。并发执行时，它控制任务开始顺序而非完成顺序。
该设置作用于下载队列；上传依赖继续使用父目录优先的操作顺序。

`download.chunk_threshold_bytes` 设置大文件阈值（字节）。超过该值的文件会通过
HTTP 字节范围请求顺序分片下载，并以该值作为单个分片的最大大小。默认值为
`8388608`（8 MiB），且必须大于零。等于或小于阈值的文件仍使用单次请求。
程序会在写入响应正文前验证 Range 响应元数据；单请求和宽松下载也会先拒绝
非成功 HTTP 响应正文，防止其进入临时文件或 durable checkpoint。大型传输
会在 Graph 内容请求中通过 `If-Match` 提交预期远端 eTag，远端版本发生变化时
会在签发下载 URL 前拒绝请求。大型传输过程中会定期可靠写盘和记录 checkpoint；
请求中断后会从最后一个安全落盘的
偏移量继续，而不是重新下载整个分片。用户正常取消时，程序也会在停止前可靠
写盘并记录已经通过 Range 响应验证的字节。

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

`permissions` 默认为 `"private"`。新同步文件使用 `0600` 创建，同步根目录和
新目录会设置为 `0700`，防止本机其他用户读取同步内容。只有确实需要通过 Unix
组权限共享同步目录时，才应设置为 `"umask"`，让权限遵循进程 umask。打包的
systemd 用户服务还会使用 `UMask=0077` 作为纵深防御。

`sync.directory` 是同步数据的公共根目录。实际 Drive 内容会使用与 state 相同的
稳定 ID 和友好名称组件进行隔离：

```text
<sync.directory>/accounts/<显示名称>--<用户-ID-哈希>/
  drives/<Drive-名称>--<Drive-ID-哈希>/
    <同步的 OneDrive 内容>
```

因此，同一个配置根目录可以同时容纳多个 Microsoft 用户和多个 Drive，且不会
发生路径冲突。旧的平面 `<sync.directory>` 布局中的文件不会自动移动，并会
保持原样。

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
