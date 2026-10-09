# Microsoft 认证

[English](authentication.md) | 简体中文 |
[文档中心](README.zh-CN.md)

CLI 采用 OAuth 2.0 设备授权流程；GUI 使用系统浏览器、授权码 + PKCE（S256）
及本地回环回调，不显示设备验证码。使用前，必须在 Microsoft Entra 中注册
自己的公共客户端应用；不要创建客户端密钥。

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

8. GUI 登录还需要打开 **身份验证 > 添加平台 > 移动和桌面应用
   （Mobile and desktop applications）**，添加 `http://localhost` 并保存。
   GUI 仅监听 IPv4 回环地址的随机端口，回调为 `http://localhost:PORT/`；
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

个人 OneDrive 账号应配置以下委托权限：

```text
User.Read
Files.ReadWrite
```

`User.Read` 用于识别当前登录账号并下载头像。客户端还会请求
`offline_access`，以便 Microsoft 返回 refresh token。只有组织版 OneDrive、
共享文档库或 SharePoint 场景确有需要时，才应添加范围更广的委托权限：

```text
Files.ReadWrite.All
Sites.Read.All
Sites.ReadWrite.All
```

只读 `inspect sites QUERY` 发现命令使用 `Sites.Read.All` 即可。只有同步过程需要修改
SharePoint 文档库时才应使用 `Sites.ReadWrite.All`。

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

单租户组织应用应将 `common` 替换为 Directory (tenant) ID。只有组织场景确实
需要时才添加 `Files.ReadWrite.All`、`Sites.Read.All` 或
`Sites.ReadWrite.All`；SharePoint 站点 scope 不支持个人 Microsoft 账号。
默认 scope 为 `User.Read Files.ReadWrite offline_access`；配置任一更广泛的
组织级 scope 时，认证会输出警告。

## 授权客户端

GUI 中点击 **Sign in**，在系统浏览器中完成登录及授权后返回应用。每次登录都会
生成新的随机 state 和 PKCE verifier，并验证回调后交换授权码。最多等待五分钟；
**Cancel login** 或关闭窗口会取消等待并回收登录线程。**Open browser** 可重新
打开当前待完成的授权页面。登录期间禁用设置，防止配置被同时修改。
授权失败或在账号激活前取消不会替换已有凭据；账号激活一旦开始，允许其完成
原子写入。

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

CLI 使用以下命令启动设备授权：

```bash
onedrive-cpp account login
```

打开终端中显示的网址，输入用户代码，再使用应用所支持的账号类型登录并确认
所请求的权限。授权成功后，客户端会读取稳定的用户身份、Drive 身份和头像，
然后将 refresh token 与头像原子写入易于识别的账号状态目录。目录权限仅允许
文件所有者访问。

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
