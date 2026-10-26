# Microsoft 认证

[English](authentication.md) | 简体中文 |
[文档中心](README.zh-CN.md)

GUI 与桌面环境下的 CLI/TUI（`--auth-flow auto` 或 `auth-code`）使用系统浏览器、
授权码 + PKCE（S256）及本地回环回调；CLI 同时支持并在无图形环境时自动回退到
OAuth 2.0 设备授权流程（`--auth-flow device-code`）。使用前，必须在 Microsoft
Entra 中注册自己的公共客户端应用；不要创建客户端密钥。

## 注册应用

Microsoft 目前要求账号具有有效的 Azure 订阅、可访问的 Microsoft Entra 租户，
以及注册应用的权限。如果只有 Outlook.com/Hotmail 个人账号，且无法打开
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

8. 浏览器登录（GUI 或 CLI `auth-code`）还需要打开 **身份验证 > 添加平台 > 移动和桌面应用
   （Mobile and desktop applications）**，添加 `http://localhost` 并保存。
   客户端仅监听 IPv4 回环地址的随机端口，回调为 `http://localhost:PORT/`；
   Microsoft 匹配 localhost 注册地址时忽略端口。不要将其注册为 Web 应用。

CLI 设备代码流不需要 Redirect URI。也不要为公共客户端添加 Client Secret，因为
桌面或命令行程序无法安全保存嵌入其中的客户端密钥。

对应的 Microsoft 官方文档：

- [在 Microsoft Entra ID 中注册应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/quickstart-register-app)
- [配置桌面和公共客户端应用](https://learn.microsoft.com/zh-cn/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 设备授权流程](https://learn.microsoft.com/zh-cn/entra/identity-platform/v2-oauth2-device-code)
- [OAuth 2.0 授权码流程](https://learn.microsoft.com/zh-cn/entra/identity-platform/v2-oauth2-auth-code-flow)

## 配置权限

打开 **API 权限（API permissions）> 添加权限（Add a permission）>
Microsoft Graph > 委托的权限（Delegated permissions）**。

个人账号或工作／学校账号访问自己的 OneDrive 时，均使用以下基础委托权限：

```text
User.Read
Files.ReadWrite
```

`User.Read` 用于识别当前登录账号并下载头像。客户端还会请求
`offline_access`，以便 Microsoft 返回 refresh token。不能仅因为 Drive 类型为
`business` 就扩大权限；共享资源和 SharePoint 功能应按实际 API 的要求申请
额外权限：

```text
Files.ReadWrite.All
Sites.Read.All
Sites.ReadWrite.All
```

只读 `inspect sites QUERY` 发现命令使用 `Sites.Read.All` 即可。修改文档库文件
不一定需要 `Sites.ReadWrite.All`，部分文件 API 也接受 `Files.ReadWrite.All`。
应逐项核对接口支持的最小权限，而不是把上述权限全部申请。

对于只允许访问指定站点的组织，`Sites.Selected` 支持委托访问，但既需要 Entra
同意，还需要为应用显式授予各目标站点的访问权限；登录用户自身的权限也仍然生效。
虽然 `Sites.Selected` 不支持全租户通配搜索（`*`），但 `inspect sites` 与 GUI
SharePoint 文档库浏览器均支持通过站点 URL、`hostname:/sites/path` 或站点 ID
直接解析已授权站点并列出其文档库。

目前 CLI 和 GUI 共用配置中的一份 `auth.scopes`，不会在用户选择 SharePoint
功能时自动申请额外权限。修改列表后，应先停止运行中的 Monitor，再重新登录，
完成必要的用户／管理员同意；GUI 需要先退出登录才能再次登录。已有 refresh
token 不能代替新增权限所需的同意。按功能触发的增量授权属于后续计划，尚未实现。

组织租户的策略可能要求管理员批准权限。个人 Microsoft 账号通常在设备登录时
由用户自行同意。最新权限定义请参阅
[Microsoft Graph 权限参考](https://learn.microsoft.com/zh-cn/graph/permissions-reference)。

## 配置 onedrive-cpp

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

单租户组织应用应将 `common` 替换为 Directory (tenant) ID；只允许组织账号的
多租户登录可使用 `organizations`，`consumers` 不接受工作／学校账号。只有组织场景确实
需要时才添加 `Files.ReadWrite.All`、`Sites.Read.All` 或
`Sites.ReadWrite.All`；SharePoint 站点 scope 不支持个人 Microsoft 账号。
默认 scope 为 `User.Read Files.ReadWrite offline_access`；配置任一更广泛的
组织级 scope 时，认证会输出警告。

## 授权客户端

GUI 中点击 **登录账号（Sign in）**，在系统浏览器中完成登录及授权后返回应用。
每次登录都会生成新的随机 state 和 PKCE verifier，并验证回调后交换授权码。
最多等待五分钟；登录期间，**退出登录（Log out）** 按钮临时切换为
**取消登录（Cancel login）**，点击它或关闭窗口会取消等待并回收登录线程。
系统浏览器会自动打开；打开失败时，可将显示的授权链接复制到浏览器中。
登录期间禁用设置，防止配置被同时修改。
授权失败或在账号激活前取消不会替换已有凭据；账号激活一旦开始，允许其完成
原子写入。

存在保存的登录凭据时，禁用 **登录账号**，启用 **退出登录**。退出仅移除当前账号
在本地保存的 refresh token，与 CLI 注销复用同一操作及运行锁；不会删除同步文件、
账号信息或同步状态，也不会退出浏览器中的微软账号。凭据移除后，只要认证配置
有效，即可再次登录。

GUI Auth Code 和 CLI Device Code 在使用相同状态目录、相同应用注册时共享账号的
refresh token。重新登录会更新该账号的共享凭据；CLI 注销也会影响 GUI。
两种登录服务都持有已有的状态目录运行锁，覆盖授权等待、令牌交换及账号激活；
CLI 刷新或删除凭据的操作使用同一把锁。并发操作会明确报错，不修改凭据。
若 CLI Monitor 正在运行，需要先停止它才能从 GUI 登录。access token 只保留在
内存；refresh token 以 0600 权限原子保存。不同 application ID 的配置不要共用
状态目录。

不依赖 Qt 的 Core `AuthCodeSession` 是单次使用状态机：
`created → awaiting_callback → authorized → exchanging_token → completed`。
取消、超时和失败均为终止状态。Core 负责 PKCE 秘密、授权 URL 和回调校验；
桌面适配层只打开浏览器和接收回环 HTTP 请求。暂未实现自定义协议回调处理器。

CLI 使用以下命令启动授权：

```bash
onedrive-cpp account login
```

默认（`--auth-flow auto`）在交互终端下探测到桌面图形环境（`DISPLAY` 或
`WAYLAND_DISPLAY` 且存在可执行的 `xdg-open`）时，优先使用本地回环监听的
OAuth 2.0 授权码 + PKCE 流程并自动打开系统浏览器；若无法启动浏览器或无图形
环境则使用／回退到设备代码流。也可通过 `--auth-flow auth-code` 或
`--auth-flow device-code` 显式指定认证方式。使用设备代码流时，打开终端中显示的网址，输入用户代码，再使用应用所支持的账号类型登录并确认
所请求的权限。授权成功后，客户端会读取稳定的用户身份、Drive 身份和头像，
然后将 refresh token 与头像原子写入易于识别的账号状态目录。目录权限仅允许
文件所有者访问。

身份查询同时读取 Graph 的 `driveType`：`personal` 表示 OneDrive 个人版，
`business` 表示组织用户自己的 OneDrive，`documentLibrary` 表示 SharePoint
文档库。类型写入 `drive.json`，在 CLI 登录／状态及 GUI 概览中显示。
读取本地元数据不会连接微软。旧元数据没有类型时显示未知，成功重新登录或同步
写入新身份信息后补齐；保留已有用户／Drive ID 目录、令牌和数据库，不把显示名或
Drive 类型当作身份键。`common`／`consumers` 只是认证入口，不代表实际租户 ID。
1.1 已支持组织用户自身 OneDrive 与已配置文档库的同步；更广泛的 SharePoint 站点
与跨租户扩展记录在 `TODO.md`。

## 组织认证诊断

CLI 登录、GUI 登录和 access token 刷新共用诊断建议。错误保留 Microsoft 返回的
说明，并在响应提供时显示 OAuth 错误及数字 `AADSTS` 诊断码。目前 Core 建议使用
英文，GUI 也直接显示；外围 GUI 控件仍按所选语言显示。
CLI 在文本和 JSON 输出模式下均沿用现有日志路径报告诊断；
`--log-level off` 会屏蔽这些日志，尚未新增独立的 JSON 认证失败事件。

| 诊断 | 建议操作 |
|---|---|
| `AADSTS90094` | 请租户管理员批准应用当前配置的委托权限。 |
| `AADSTS65001` 或 `consent_required` | 重新登录完成同意；只有组织策略要求时才必须由管理员批准。 |
| `AADSTS65004` | 用户拒绝同意；确认所请求的权限后再授权。 |
| `AADSTS50076`、`50079`、`50097`、`53000`–`53003` | 完成必要的认证／设备检查，并请管理员核查条件访问和受批准设备／应用要求。 |
| `AADSTS530036` | 条件访问限制了此授权的认证流程，例如设备代码流或 authentication transfer。停止 Monitor，显式退出本地登录，仅在组织允许时改用 GUI 浏览器登录。 |
| `interaction_required` 或 `login_required` | 停止 Monitor，重新交互登录；GUI 需要先退出本地登录。 |
| `invalid_grant`，包括授权过期／撤销诊断 | 停止 Monitor，运行 `onedrive-cpp account login`，或先退出本地登录再从 GUI 登录。认证失败本身不会自动删除已保存的凭据。 |
| `unauthorized_client` 或 `unsupported_grant_type` | 核对配置租户、支持的账号类型及 **Allow public client flows**，不要添加客户端密钥。 |
| `temporarily_unavailable` 或 `server_error` | 稍后重试；不要仅因服务暂时不可用就扩大权限或删除凭据。 |

浏览器登录不能绕过组织策略；尚未实现自动 claims challenge 处理或设备 broker／
合规设备认证。未知诊断码不能作为某项组织策略失败的确证。设备轮询的控制依据
仍然是 OAuth 错误标识，数字 STS 码只补充建议，不会自动切换授权流程或删除令牌，
也不会为解决这些错误额外申请权限。

显示的 **Configured tenant/selector (not a verified tenant ID)** 来自配置
`auth.tenant_id`，不是已验证的实际租户。特别是 `common`、`organizations` 和
`consumers` 只是选择入口，不代表本次登录实际使用的目录。客户端不会解码未验证的
access token 来猜测目录；请管理员通过 Microsoft Entra 登录日志核对实际租户及
策略判定。真实组织租户同步验收由 `tests/business_e2e.py` 覆盖。

可参阅 Microsoft 的[认证错误参考](https://learn.microsoft.com/zh-cn/entra/identity-platform/reference-error-codes)
和 [530036 诊断](https://login.microsoftonline.com/error?code=530036)。
反馈认证问题时，不要提供授权码、令牌或完整回调 URL。

## 常见问题

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

重新运行 `onedrive-cpp account login`，并且只在新显示的
`https://login.microsoft.com/device` 页面中使用本次新代码。之前生成的设备代码
不能重复使用。只有应用的 Supported account type 确实为
**Personal Microsoft accounts only** 时才使用 `consumers`。

如果设备代码已被接受并进入账号登录，但之后 Microsoft 又提示代码已过期，
同时终端仍停留在 `Waiting for authorization...`，应删除当前账号类型不支持的
权限。个人 Microsoft 账号应使用：

```toml
[auth]
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

修改权限后必须重新运行 `onedrive-cpp account login`；已有设备代码仍绑定原来的权限，
无法修复或重复使用。

使用以下命令删除已保存的认证：

```bash
onedrive-cpp account logout
```
