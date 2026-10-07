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
onedrive-cpp transfer download Documents/report.pdf --ui tui
onedrive-cpp transfer sync --dry-run --output json
onedrive-cpp transfer sync --dry-run --quiet
onedrive-cpp inspect drives --output json
onedrive-cpp inspect shared
onedrive-cpp inspect sites Engineering
onedrive-cpp inspect quota
onedrive-cpp inspect status
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

`account login`、`inspect health`、`inspect status`、`transfer sync`、`transfer download` 和 `transfer watch` 读取配置中的 `console.ui`，默认值为 `auto`；
命令行显式提供的 `--ui` 只覆盖本次运行。auto 模式只有在标准输入和标准输出均连接终端、`TERM`
支持终端控制，并且窗口至少为 60 列 × 12 行时，才会打开 FTXUI 实时状态面板。
输出重定向、管道、JSON、quiet 模式、`TERM=dumb` 或窗口过小时，会自动切回普通
Console。使用 `--ui=console` 可以始终关闭面板；`--ui=tui` 则会强制启用面板，
若当前终端不支持，会明确报错而不是静默回退。在 watch 面板中按 `q`、`Q` 或
`Esc` 可以安全退出。

面板启动后会立即进入终端的全屏备用缓冲区，标题显示客户端版本，退出时恢复原屏幕。
download 面板复用同步期间持久显示的进度、传输速率和 ETA。同步中断后重试时，
文件数和字节进度会包含已经完成并验证的下载，速度与 ETA 则只计算本轮新传输。
界面状态使用“云端”“文件”等用户化措词，不直接展示底层 API 名称。`console.theme`
可选择默认的荧光绿 `hacker`、青蓝 `ocean`、琥珀 `amber` 或洋红/青色
`synthwave`；`--theme` 只覆盖本次运行。`--color=never` 和 `NO_COLOR` 会保留
所选布局，但关闭主题颜色。

## 只读账号与同步信息

`inspect drives` 列出当前 Microsoft 账号可用的 OneDrive Drive，并标记配置正在使用的
Drive。`inspect shared` 会列出 `sharedWithMe` 返回的项目，以及已添加到当前 OneDrive 的
快捷方式（shortcut）；同时显示配置共享 Drive 所需的目标 Drive ID 和 item ID。
`inspect sites QUERY` 搜索可访问的 SharePoint 站点并列出各站点的文档库 Drive。
站点发现要求 `Sites.Read.All` 或 `Sites.ReadWrite.All`；修改 scope 后需要重新
运行 `onedrive-cpp account login`。

`inspect quota` 显示配置 Drive 的总量、已用、剩余、回收站占用和配额状态。`inspect status`
合并当前账号及规范 Drive 身份与本地只读状态，包括同步模式、删除策略、最后一次
同步结果、tracked/blocked 数量、pending journal、Delta cursor、
selective-sync fingerprint 和 WebSocket 配置。

这些命令不会同步文件或修改远端内容。`inspect status` 以只读方式打开现有 SQLite 数据库，不会创建缺失的数据库，也不会迁移
旧版数据库。以上五个命令均支持 `--output json`。

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
