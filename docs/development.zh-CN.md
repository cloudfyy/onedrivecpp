# 开发与测试

[English](development.md) | 简体中文 |
[文档中心](README.zh-CN.md)

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

策略只排除经过审查的必要 C/POSIX API、协议常量和已检查缓冲区边界噪声。
项目使用 Microsoft GSL 在 API 和 RAII 边界表达非空借用依赖，并使用
ngcpp/proxy 提供具有明确拥有/借用适配方式的类型擦除运行时端口。

协议标识和日志级别应优先复用不依赖 locale 的 ASCII 比较，而非复制字符串后
转小写。同步借用文本使用 `std::string_view`，剩余 I/O 缓冲区使用 `std::span`
子视图。POSIX 调用继续由 RAII 对象管理；新语法不能削弱关闭错误检查、持久化
和错误报告行为。

测试通过 `tests/support/common.hpp` 复用带错误检查的二进制文件 I/O 和异常匹配。
`throws_with<Exception>(operation, message)` 按消息子串匹配；未抛异常时返回 false，
指定类型之外的异常继续传播。测试不应接受逻辑错误时，应指定 `std::runtime_error`。
callable 采用借用方式，支持不可复制对象。`test_support_tests` 独立验证这些契约。
