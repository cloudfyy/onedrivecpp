# 开发与测试

[English](development.md) | 简体中文

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

稳定入口 `graph_sync_e2e.py` 会委托给 `tests/e2e/` 中的 Python 模块；真实
Graph 场景、系统边界场景、进程编排、状态 fixture 和 runner 自测分别维护。

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

## C++ Core Guidelines 检查

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
