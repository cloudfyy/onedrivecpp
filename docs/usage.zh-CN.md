# 使用与命令输出

[English](usage.md) | 简体中文

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
onedrive-cpp drives --output json
onedrive-cpp quota
onedrive-cpp status
```

支持 `trace`、`debug`、`info`、`warn`、`error`、`critical` 和 `off`。
日志文件达到 5 MiB 时轮转，并保留三个旧文件。不得把认证 token、设备代码或
Authorization header 写入日志。

`--color=auto` 是默认值：仅在终端中启用样式，设置 `NO_COLOR` 时自动禁用；
`always` 强制输出 ANSI 样式，`never` 始终禁用。`--output=json` 每行输出一个
紧凑 JSON 对象且绝不包含 ANSI 序列；危险操作在该模式下必须使用 `--yes`
显式确认。`--quiet` 隐藏普通信息和成功消息，但保留警告和错误。诊断日志继续
写入标准错误，命令结果写入标准输出。

## 只读账号与同步信息

`drives` 列出当前 Microsoft 账号可用的 OneDrive Drive，并标记配置正在使用的
Drive。`quota` 显示配置 Drive 的总量、已用、剩余、回收站占用和配额状态。
`status` 合并当前账号及规范 Drive 身份与本地只读状态，包括同步模式、删除策略、
最后一次同步结果、tracked/blocked 数量、pending journal、Delta cursor、
selective-sync fingerprint 和 WebSocket 配置。

这些命令不会同步文件或修改远端内容。`status` 以只读方式打开已有 SQLite
数据库，不会创建或迁移缺失或旧版本数据库。三个命令均支持 `--output json`。

## Shell 自动补全

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
