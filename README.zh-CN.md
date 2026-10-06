# onedrive-cpp

[English](README.md) | 简体中文

`onedrive-cpp` 是一个面向 Ubuntu 24.04 LTS 及以上版本的 C++26 OneDrive
同步客户端。它参考
[abraunegg/onedrive](https://github.com/abraunegg/onedrive) 的职责拆分方式，
但不复制其 D 语言实现。

## 功能

- OAuth 2.0 设备代码认证，以及安全隔离的账号 Token 和资料状态。
- Microsoft Graph Delta 增量同步、多账号和多 Drive 隔离、选择性同步规则及
  显式单文件下载。
- 并发、可续传且带完整性校验的下载，支持原子安装、磁盘空间协调、总速率限制，
  以及阻止或备份本地冲突。
- 并发简单上传和 upload session，支持稳定快照、持久 checkpoint、崩溃恢复、
  单文件及总速率限制和条件 Graph 写入。
- 文件与目录的双向创建、修改、删除、移动和重命名传播，包括依赖环 staging 和
  可配置的大批量远端删除保护。
- 基于 SQLite journal 的下载、上传、本地与远端移动、远端删除和目录创建恢复。
- inotify 长驻监控、Graph 轮询、事件合并、队列溢出恢复和干净信号退出。
- dry-run、结构化 JSON 输出、轮转日志、代理、Shell 自动补全、手册页、加固的
  systemd 用户服务和 Debian 打包。

## 架构

```mermaid
flowchart TB
    main["main<br/>唯一组合根"] --> app["Application<br/>CLI、预检与命令"]
    app --> runtime["RuntimeFactory<br/>Proxy 4 端口"]
    runtime -. 由其实现 .-> adapters["生产适配器"]

    app --> auth["认证 / 登出 / 诊断"]
    app --> monitor["Monitor 状态机"]
    app --> engine["SyncEngine"]

    subgraph sync["双向同步"]
        recover["恢复持久操作"]
        delta["Graph Delta 与远端计划"]
        remote["应用远端变化<br/>移动、删除、建目录、下载"]
        scan["扫描本地快照"]
        local["应用本地变化<br/>删除、移动、建目录、上传"]
        recover --> delta --> remote --> scan --> local
    end

    engine --> recover
    monitor --> engine
    remote --> leases["按条目加锁的操作协调"]
    local --> leases
    remote --> transfers["并发传输 worker<br/>空间与速率协调"]
    local --> transfers

    adapters --> graph["MicrosoftGraphClient"]
    adapters --> http["CurlHttpClient"]
    adapters --> store["ItemDatabase"]
    adapters --> files["文件系统与 inotify"]
    graph --> cloud[("Microsoft Graph / OneDrive")]
    graph --> http
    remote --> graph
    local --> graph
    remote --> files
    local --> files
    recover --> store
    delta --> store
    remote --> store
    local --> store
    store --> db_thread["专用数据库线程"]
    db_thread --> sqlite[("SQLite 快照、游标、<br/>journal 与 blocked item")]
```

应用使用构造器注入和明确的端口接口，不使用 Service Locator。`main` 是唯一的
composition root。生产 runtime factory 在配置加载后创建 libcurl、文件 token、
SQLite、monitor、Graph 和 metrics 适配器；测试则注入内存 fake。这样业务编排
不再依赖基础设施实现，instrumentation 也通过稳定端口与编排解耦。
运行时端口使用 Proxy 4 类型擦除，因此适配器只需满足所需操作，无需继承项目
定义的抽象基类。
SQLite ItemStore 拥有专用数据库线程。下载、监控和上传工作线程发起的调用
都会排队并同步等待完成，因此 SQLite 连接和事务顺序始终由一个线程负责，错误
则返回给调用方。SQLite 是状态的权威来源，适配器不再维护重复的可变条目缓存。
下载和上传事务复用一个小型模板 Typestate 核心，用于隔离状态族并在合法阶段间
移动 payload。下载包含内容已验证和恢复 journal 已持久化阶段；上传包含快照已
准备、journal 已持久化和远端已提交阶段，并且只有 journaled 上传可以持久化
Graph 会话 checkpoint。公共模板不包含 Graph、SQLite 或文件系统策略：Typestate
负责约束进程内转换，SQLite 仍然是持久化恢复的权威来源。
每种事务都在公共底层原语之上提供精确类型的命名转换边。转换可以映射 payload
类型，因此后续状态只保存有效数据：journaled 上传不再携带已释放的 snapshot，
Graph 已提交的远端移动直接拥有必需的远端条目，而不是 optional 值。
本地移动恢复也复用该核心，表达 prepared、journaled、恢复 journal、staged 和
installed 阶段。只有类型状态能够证明尚未生成 staging 或目标对象时，失败路径
才会删除 journal；后续阶段始终保留恢复证据供重启使用。
远端移动使用独立状态族表达 prepared、journaled、Graph 已提交和本地已提交阶段。
只有 journal 持久化后才能调用 Microsoft Graph；远端移动成功后仍保留 journal，
直到本地条目状态和目录后代路径完成原子提交。
远端删除同样使用 prepared、journaled、Graph 已删除和本地已提交状态。journal
写入失败时不能调用 Graph；Graph 删除完成后继续保留 journal，直到本地跟踪子树
完成原子删除。
远端目录创建使用独立的 prepared、journaled、Graph 已创建和本地已提交状态族。
新建和重启恢复统一进入同一个 Graph 已创建提交路径，复用本地目录验证、inode
获取、SQLite 提交和远端身份 metadata 写入。
Graph 大文件上传会话也在同步层之外复用此核心。不存在或已保存的会话只能通过
创建或验证恢复进入 active；已过期、不存在或已失效的保存会话先返回 absent，
再创建新会话。每个已接受的分片只有在 checkpoint 成功后才推进 active 状态，
并且只有 active 会话能够生成包含远端条目的 finalized 状态。

目录与参考项目中的 `main/config/curlEngine/onedrive/sync/itemdb/monitor`
职责相对应：

- `src/app`：CLI 解析、应用生命周期和运行时依赖工厂。
- `src/account`：稳定账号与 Drive 身份、元数据和路径。
- `src/auth`：设备代码 OAuth、Token 刷新和安全持久化。
- `src/cli`：文本和 JSON 用户输出。
- `src/config`：配置文件加载和校验。
- `src/graph`：Microsoft Graph API 访问边界。
- `src/http`：强类型 libcurl HTTP 传输层。
- `src/logging`：运行时诊断日志。
- `src/storage`：ItemStore 端口，以及使用 SQLite 持久化远端 ID、ETag 与本地路径的适配器。
- `src/sync`：计划、下载、上传、文件系统、筛选和恢复操作族。
- `src/monitor`：长驻监控状态机和 inotify 适配器。
- `src/metrics`：Metrics 端口和无操作生产适配器。
- `packaging/systemd`：systemd 用户服务。
- `debian`：Ubuntu/Debian 原生包元数据。

## 支持范围

- Ubuntu 24.04 LTS 及更新版本。
- x86_64 或 arm64（取决于 LLVM 和 Ubuntu 构建环境）。
- Clang 20，使用 `-std=c++2c`/CMake `CXX_STANDARD 26`。
- CMake 3.28 或更高版本。
- 使用系统包管理器提供 CLI11、libcurl/OpenSSL、nlohmann/json、
  spdlog/fmt、SQLite 和 toml++ 开发包。

Ubuntu 24.04 的官方仓库已经提供项目所需的 CMake、Ninja 和 Clang 20。
项目不使用默认的 GCC 13，因为它的 C++26 支持不足以满足当前配置。

## 配置构建环境

从 Ubuntu 24.04 官方仓库安装构建工具：

```bash
sudo apt update
sudo apt install -y build-essential ca-certificates curl git \
  cmake ninja-build clang-20 clang-format-20 clang-tidy-20 \
  zip unzip tar pkg-config \
  libcli11-dev libcurl4-openssl-dev libfmt-dev libspdlog-dev \
  libmsgsl-dev libsqlite3-dev libssl-dev libtomlplusplus-dev \
  nlohmann-json3-dev
```

确认版本：

```bash
clang++-20 --version
clang-format-20 --version
cmake --version
```

提交前必须格式化本次修改的 C/C++ 行。先暂存源文件和头文件，再从仓库根目录
运行固定版本的 Git 集成，使其读取仓库内的 `.clang-format`，同时避免重排无关
的历史代码：

```bash
git add path/to/modified.cpp path/to/modified.hpp
git-clang-format-20 --binary clang-format-20 --staged
git add path/to/modified.cpp path/to/modified.hpp
```

新文件可以直接使用 `clang-format-20 -i` 完整格式化。不要使用无版本后缀的
命令或其他主版本，因为输出可能与 Clang 20 不一致。

编译库从操作系统解析并动态链接。这样 Debian 的安全更新可以替换 libcurl、
OpenSSL、SQLite、spdlog 和 fmt，而无需重新构建 `onedrive-cpp`。
Proxy 4 是 header-only 库。CMake 优先使用已安装的 `msft_proxy4` 包，否则下载
固定版本的 ngcpp/proxy 4.1.0；vcpkg manifest 则通过 `proxy` port 解析它。

## 本地构建与测试

克隆仓库后请进入仓库根目录。以下所有构建命令均假定当前工作目录为项目根目录。

### Release 构建

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

生成的程序位于 `build/release/onedrive-cpp`。使用以下命令验证：

```bash
./build/release/onedrive-cpp --version
./build/release/onedrive-cpp sync --dry-run
./build/release/onedrive-cpp --help
```

### 日志

运行诊断默认以 `info` 级别写入标准错误。systemd 会自动收集该输出：

```bash
journalctl --user -u onedrive-cpp.service -f
```

每个子命令都支持诊断日志和用户输出选项：

```bash
onedrive-cpp sync --dry-run --log-level debug
onedrive-cpp monitor --log-file ~/.local/state/onedrive-cpp/onedrive-cpp.log
onedrive-cpp sync --dry-run --color always
onedrive-cpp sync --dry-run --output json
onedrive-cpp sync --dry-run --quiet
```

支持 `trace`、`debug`、`info`、`warn`、`error`、`critical` 和 `off`。
日志文件达到 5 MiB 时轮转，并保留三个旧文件。不得把认证 token、设备代码或
Authorization header 写入日志。

`--color=auto` 是默认值：仅在终端中启用样式，设置 `NO_COLOR` 时自动禁用；
`always` 强制输出 ANSI 样式，`never` 始终禁用。`--output=json` 每行输出一个
紧凑 JSON 对象且绝不包含 ANSI 序列；危险操作在该模式下必须使用 `--yes`
显式确认。`--quiet` 隐藏普通信息和成功消息，但保留警告和错误。诊断日志继续
写入标准错误，命令结果写入标准输出。

### Shell 自动补全

安装包会安装 Bash 和 Zsh 补全定义，支持子命令、选项、枚举值和文件路径。
启用 shell completion 的新终端会自动加载。

开发构建可在当前 Bash 中执行：

```bash
source packaging/completions/onedrive-cpp.bash
```

随后可以使用 Tab 补全：

```bash
build/release/onedrive-cpp <Tab>
build/release/onedrive-cpp sync --<Tab>
build/release/onedrive-cpp sync --log-level <Tab>
```

安装位置分别为 `share/bash-completion/completions/onedrive-cpp` 和
`share/zsh/vendor-completions/_onedrive-cpp`。

### 手册页

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

### 完整 Release 重编译

仅删除 Release 构建目录，使 CMake 和 Ninja 从空白状态重新配置和编译：

```bash
rm -rf build/release
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/onedrive-cpp sync --dry-run
```

Ninja 默认自动并行编译。如需明确指定并行任务数：

```bash
cmake --build --preset release --parallel 4
```

### Debug 构建

Debug 与 Release 使用相互独立的构建目录：

```bash
rm -rf build/debug
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Debug 程序位于 `build/debug/onedrive-cpp`。

### Microsoft Graph 真实 E2E 测试

普通构建不会启用 `e2e` preset。该测试需要专用测试账号或 Drive，并在远端根目录
预置内容稳定的 fixture。仓库外配置必须已完成认证，且绝不能提交到仓库。runner
会把状态目录复制到隔离的临时工作区，只清理该副本，并配置隔离的 `sync_list`。
它会验证显式单文件下载可以绕过空规则文件，随后的同步排除其快照但不删除本地
文件，加入包含规则会触发完整远端状态查询并重新下载 fixture，最后一次增量同步
不会替换未变化的文件。随后 runner 会写入冲突的本地内容，只清理隔离状态副本，
并验证 `sync.local_conflict = "backup"` 发出结构化事件、把本地字节完整保存为同
目录下唯一的 `safeBackup`、恢复 Graph 权威 fixture、持久化一条快照，而且后续
增量同步不会改写远端文件或备份。runner 会强制隔离配置使用 `backup` 策略，不会
修改外部配置。它还会创建可丢弃的本地子树，验证远端目录创建、简单上传、超过
250 MB 的 upload session、已完成上传恢复、本地和远端同时修改时的冲突备份、
移动到新父目录以及本地删除。大文件默认是 250,000,001 字节；
`ONEDRIVE_E2E_LARGE_UPLOAD_BYTES` 可以把它增大到 1 GB，但不能降到 upload
session 阈值以下。随后它使用复制账户中的 token 创建另一个名称唯一、可丢弃的
Graph 子树，把一个文件和一个目录跨远端父目录移动并重命名，验证同步复用相同
的本地 inode、更新快照并发出结构化移动事件；即使测试失败，远端临时子树也会
被删除。
最后，它会向隔离状态注入一条可信快照；该 ID 不存在于真实完整 Graph Delta
响应中，并验证安全本地删除、SQLite 清理、结构化输出以及真实 fixture 仍保持
不变。它还会启用 `sync_root_files`，验证选择摘要变化会触发完整 Graph 查询，
并确认已有规则选中的 fixture 不会被重写。

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

专用 Drive 必须授予文件写权限，并且只应包含可丢弃的测试数据；runner 生成的
`sync_list` 只会落地预期 fixture 和临时 fixture。应预留足够的本地空间同时保存
大文件源和稳定上传快照，并确保测试账号有足够配额保存远端副本。设置
`ONEDRIVE_E2E_ARTIFACT_DIR` 后，失败时会保留命令输出和客户端日志；
这些诊断信息可能包含远端文件元数据，应按敏感数据保管。临时配置、复制的 token、
SQLite 状态及下载内容始终会删除。不要让 E2E runner 使用日常状态目录或日常 Drive。

live runner 还会使用真实可执行文件验证系统边界：本地代理主动断开连接及恢复、
确定性的本地上传存储耗尽及恢复、不可读上传源、可续传上传期间的 `SIGKILL`，
以及由 inotify 触发的 Monitor 上传和正常 `SIGTERM` 退出。测试必须以非 root
用户运行，因为权限场景依赖普通 Unix 访问检查。CMake 还要求提供 `stdbuf`
命令，以便在不改变生产输出缓冲行为的情况下观察 Monitor JSON 事件。

每个边界场景都是独立的 CTest 条目，可通过以下命令运行全部五项：

```bash
ctest --test-dir build/e2e --output-on-failure -L boundary
```

### C++ Core Guidelines 检查

`lint` preset 会在编译时按照项目的 `.clang-tidy` 策略运行 Clang-Tidy。
Clang 静态分析器、bug-prone、performance、portability 以及选定的 C++ Core
Guidelines 诊断都会作为构建错误：

```bash
cmake --preset lint
cmake --build --preset lint
```

策略只排除经过审查的必要 C/POSIX API、协议常量和已检查缓冲区边界噪声。
项目使用 Microsoft GSL 在 API 和 RAII 边界表达非空借用依赖，并使用
ngcpp/proxy 提供具有明确拥有/借用适配方式的类型擦除运行时端口。

## 构建 DEB 包

### 快速生成 CPack 包

使用 CPack 快速生成未经过完整 Debian 策略检查的包：

```bash
cpack --config build/release/CPackConfig.cmake -G DEB
```

生成的文件位于当前目录。CMake 会从 `/etc/os-release` 读取 `ID` 和
`VERSION_ID`，因此 Ubuntu 24.04 amd64 构建命名为：

```text
onedrive-cpp_0.8.0-1~ubuntu24.04_amd64.deb
```

在跨发行版构建环境中，可在配置时显式覆盖检测结果：

```bash
cmake --preset release \
  -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04
```

Ubuntu 26.04 包仍必须在 Ubuntu 26.04 环境中构建和测试；仅修改后缀不会改变
二进制 ABI。

### 构建正式 Debian 包

安装打包依赖：

```bash
sudo apt install -y build-essential devscripts debhelper ninja-build clang-20
```

在项目根目录执行：

```bash
source "$HOME/.profile"
chmod +x debian/rules
dpkg-buildpackage --build=binary --no-sign
```

`dpkg-buildpackage` 会执行 CMake 配置、编译和测试。生成的 `.deb` 位于项目
目录的上一级，例如：

```text
../onedrive-cpp_0.8.0-1~ubuntu24.04_amd64.deb
```

Debian changelog 保存原生构建发行版后缀。将来增加 Ubuntu 26.04 支持时，应在
Ubuntu 26.04 环境中使用 `0.8.0-1~ubuntu26.04` changelog 版本构建。

安装并检查：

```bash
version=$(dpkg-parsechangelog -S Version)
architecture=$(dpkg --print-architecture)
sudo apt install "../onedrive-cpp_${version}_${architecture}.deb"
onedrive-cpp --version
man onedrive-cpp
```

如需检查 Debian 策略问题：

```bash
sudo apt install -y lintian
lintian "../onedrive-cpp_${version}_${architecture}.changes"
```

## 配置与 systemd

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

### Microsoft 认证

客户端使用 OAuth 2.0 设备授权流程。必须在 Microsoft Entra 中注册自己的公共
客户端应用；不要创建客户端密钥。

#### 注册应用

Microsoft 当前要求账号具有有效的 Azure 订阅、可访问的 Microsoft Entra 租户，
并拥有注册应用的权限。如果只有 Outlook.com/Hotmail 个人账号且无法打开
**App registrations**，请先创建
[免费 Azure 账号](https://azure.microsoft.com/zh-cn/pricing/purchase-options/azure-account)，
使用其 **Default Directory**，或者请租户管理员分配 Application Developer
角色。

1. 登录 [Microsoft Entra 管理中心](https://entra.microsoft.com/)。
2. 打开 **Entra ID > 应用注册（App registrations）> 新注册
   （New registration）**。
3. 输入应用名称，例如 `onedrive-cpp`。
4. 选择支持的账号类型：
   - 如果同时支持工作/学校账号和个人 Microsoft 账号，选择
     **Any Entra ID Tenant + Personal Microsoft accounts**，并在配置中使用
     `auth.tenant_id = "common"`。
   - 如果只支持个人 Microsoft 账号，选择 **Personal accounts only**，并使用
     `auth.tenant_id = "consumers"`。
   - 如果只供一个组织使用，选择 **Single tenant**，并使用该目录的 Tenant ID。
5. 点击 **注册（Register）**。
6. 在应用的 **概述（Overview）** 页面复制 **Application (client) ID**。
   `auth.application_id` 不应填写 Object ID 或 Directory ID。
7. 打开 **身份验证（Authentication）> 高级设置（Advanced settings）**，
   将 **Allow public client flows** 设置为 **Yes** 并保存。

设备代码流不需要 Redirect URI。公共客户端中也不要添加 Client Secret，因为
桌面或命令行程序无法安全保存嵌入的客户端密钥。

对应的 Microsoft 官方文档：

- [在 Microsoft Entra ID 中注册应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/quickstart-register-app)
- [配置桌面和公共客户端应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 设备授权流程](https://learn.microsoft.com/zh-cn/entra/identity-platform/v2-oauth2-device-code)

#### 配置权限

打开 **API 权限（API permissions）> 添加权限（Add a permission）>
Microsoft Graph > 委托的权限（Delegated permissions）**。

个人 OneDrive 账号应配置以下委托权限：

```text
User.Read
Files.ReadWrite
```

`User.Read` 用于识别当前登录账号和下载头像。客户端还会请求
`offline_access`，以便 Microsoft 返回 refresh token。只有在
组织版 OneDrive、共享文档库或 SharePoint 场景确实需要时，才添加更广泛的
委托权限：

```text
Files.ReadWrite.All
Sites.ReadWrite.All
```

组织租户的策略可能要求管理员批准权限。个人 Microsoft 账号通常在设备登录时
由用户自行同意。最新权限定义请参阅
[Microsoft Graph 权限参考](https://learn.microsoft.com/zh-cn/graph/permissions-reference)。

#### 配置 onedrive-cpp

如果用户配置文件尚不存在，先创建：

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

注册类型为 **仅个人 Microsoft 账号（Personal Microsoft accounts only）**
的应用使用：

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "consumers"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

如果应用注册类型为 **任何 Entra ID 租户和个人 Microsoft 账号
（Any Entra ID Tenant + Personal Microsoft accounts）**，即使本次登录使用
个人账号，也应使用 `common`：

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "common"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

单租户组织应用应将 `common` 替换为 Directory (tenant) ID。只有组织场景确实
需要时才添加 `Files.ReadWrite.All` 或 `Sites.ReadWrite.All`；
`Sites.ReadWrite.All` 不支持个人 Microsoft 账号。默认 scope 为
`User.Read Files.ReadWrite offline_access`；配置任一更广泛的组织级 scope 时，
认证会输出警告。

#### 授权客户端

使用以下命令启动设备授权：

```bash
onedrive-cpp auth
```

打开终端显示的网址，输入用户代码，使用与应用支持账号类型相符的账号登录，
并确认所请求的权限。成功后，客户端会读取稳定的用户和 Drive 身份以及头像，
并将 refresh token 和头像原子保存到友好的账号状态目录，权限限制为仅文件
所有者可访问。

如果 Microsoft 提示账号类型不受支持，请检查应用注册中的
**Supported account types**：仅个人账号注册应使用 `consumers`，同时支持个人
和组织账号的注册应使用 `common`。

如果程序显示 `https://www.microsoft.com/link`，但该页面立即提示刚生成的代码
无效或已过期，请检查是否把同时支持个人和组织账号的应用错误配置成了
`auth.tenant_id = "consumers"`。应改为：

```toml
[auth]
tenant_id = "common"
```

重新运行 `onedrive-cpp auth`，并且只在新显示的
`https://login.microsoft.com/device` 页面中使用本次新代码。之前生成的设备代码
不能重复使用。只有应用的 Supported account type 确实是
**Personal Microsoft accounts only** 时才使用 `consumers`。

如果设备代码已被接受并进入账号登录，但之后 Microsoft 又提示代码已过期，
同时终端仍停留在 `Waiting for authorization...`，应删除当前账号类型不支持的
权限。个人 Microsoft 账号应使用：

```toml
[auth]
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

修改权限后必须重新运行 `onedrive-cpp auth`；已有设备代码仍绑定原来的权限，
无法修复或重复使用。

使用以下命令删除已保存的认证：

```bash
onedrive-cpp logout
```

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

## 许可证

GPL-3.0-or-later。参考项目同样采用 GPLv3，因此该许可证也便于未来在满足
许可证要求的前提下复用或改写其设计。

## 法律与品牌材料

- [服务条款](TERMS.md)
- [隐私声明](PRIVACY.md)
- 项目 Logo：[SVG](assets/onedrive-cpp-logo.svg) |
  [512x512 PNG](assets/onedrive-cpp-logo.png)

法律文件是项目维护者草案，不构成法律建议。正式公开或商业发布前，应根据
实际运营主体和适用司法管辖区进行审核。onedrive-cpp 是独立项目，与 Microsoft
不存在隶属或背书关系。
