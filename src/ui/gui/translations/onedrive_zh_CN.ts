<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN" sourcelanguage="en">
<context>
    <name>AccountStatus</name>
    <message>
        <location filename="../localization.cpp" line="134"/>
        <source>Account status unavailable</source>
        <translation>账号状态不可用</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="138"/>
        <source>Local account credentials need attention</source>
        <translation>本地账号凭据需要处理</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="122"/>
        <source>No active account</source>
        <translation>无活动账号</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="126"/>
        <source>Account credentials available</source>
        <translation>账号凭据可用</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="130"/>
        <source>Active account has no saved credentials</source>
        <translation>活动账号没有已保存的凭据</translation>
    </message>
</context>
<context>
    <name>Application</name>
    <message>
        <location filename="../main.cpp" line="59"/>
        <source>Cannot start OneDrive client:
%1</source>
        <translation>无法启动 OneDrive 客户端：
%1</translation>
    </message>
</context>
<context>
    <name>BrowserLogin</name>
    <message>
        <location filename="../login_controller.cpp" line="35"/>
        <source>Browser login requires a browser callback.</source>
        <translation>浏览器登录需要浏览器回调。</translation>
    </message>
    <message>
        <location filename="../login_controller.cpp" line="48"/>
        <source>Cannot listen for browser authorization: %1</source>
        <translation>无法监听浏览器授权回调：%1</translation>
    </message>
    <message>
        <location filename="../login_controller.cpp" line="63"/>
        <source>Authorization received. Return to OneDrive client. You may close this tab.</source>
        <translation>已收到授权。请返回 OneDrive 客户端。可以关闭此标签页。</translation>
    </message>
    <message>
        <location filename="../login_controller.cpp" line="69"/>
        <source>Invalid authorization callback.</source>
        <translation>无效的授权回调。</translation>
    </message>
    <message>
        <location filename="../login_controller.cpp" line="89"/>
        <source>Browser authorization listener failed: %1</source>
        <translation>浏览器授权监听失败：%1</translation>
    </message>
    <message>
        <location filename="../login_controller.cpp" line="109"/>
        <source>Invalid authorization callback HTTP request.</source>
        <translation>无效的授权回调 HTTP 请求。</translation>
    </message>
</context>
<context>
    <name>Configuration</name>
    <message>
        <location filename="../localization.cpp" line="148"/>
        <source>Bidirectional</source>
        <translation>双向同步</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="150"/>
        <source>Upload only</source>
        <translation>仅上传</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="152"/>
        <source>Download only</source>
        <translation>仅下载</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="160"/>
        <source>Propagate deletions</source>
        <translation>传播删除操作</translation>
    </message>
    <message>
        <location filename="../localization.cpp" line="164"/>
        <source>Preserve destination files</source>
        <translation>保留目标文件</translation>
    </message>
</context>
<context>
    <name>onedrive::gui::LoginController</name>
    <message>
        <location filename="../login_controller.cpp" line="270"/>
        <source>Login is already running.</source>
        <translation>登录已在进行中。</translation>
    </message>
</context>
<context>
    <name>onedrive::gui::MainWindow</name>
    <message>
        <location filename="../main_window.cpp" line="114"/>
        <location filename="../main_window.cpp" line="287"/>
        <source>Reload configuration</source>
        <translation>重新加载配置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="420"/>
        <source>Sign in is unavailable. Resolve the errors in Settings, then reload the configuration.</source>
        <translation>暂时无法登录。请在设置中解决错误，然后重新加载配置。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="425"/>
        <source>Saved credentials have not been verified online. No sign-in is required at startup.</source>
        <translation>已保存的凭据尚未经在线验证。启动时无需登录。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="437"/>
        <source>Configuration file is missing. Using defaults; complete setup in Settings.</source>
        <translation>配置文件不存在。正在使用默认值；请在设置中完成配置。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="442"/>
        <source>Configuration could not be loaded:
%1
Import a valid TOML file, or repair this file and reload. The original file has not been changed.</source>
        <translation>无法加载配置：
%1
请导入有效的 TOML 文件，或修复此文件后重新加载。原文件未被修改。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="449"/>
        <source>Authentication configuration needs attention:
%1</source>
        <translation>身份验证配置需要处理：
%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="453"/>
        <source>Local account credentials could not be read:
%1
Repair the state directory or select another one in Settings, then reload.</source>
        <translation>无法读取本地账号凭据：
%1
请修复状态目录或在设置中选择其他目录，然后重新加载。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="460"/>
        <source>Sync directory needs attention before synchronization:
%1</source>
        <translation>同步前需要处理同步目录：
%1</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="39"/>
        <location filename="../main_window.cpp" line="48"/>
        <location filename="../main_window.cpp" line="261"/>
        <source>onedrive client</source>
        <translation>OneDrive 客户端</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="289"/>
        <source>Browse…</source>
        <translation>浏览…</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="60"/>
        <location filename="../main_window.cpp" line="272"/>
        <source>Account</source>
        <translation>账号</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="61"/>
        <location filename="../main_window.cpp" line="96"/>
        <location filename="../main_window.cpp" line="273"/>
        <location filename="../main_window.cpp" line="278"/>
        <source>Configuration file</source>
        <translation>配置文件</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="62"/>
        <location filename="../main_window.cpp" line="274"/>
        <source>Sync directory</source>
        <translation>同步目录</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="63"/>
        <location filename="../main_window.cpp" line="275"/>
        <source>State directory</source>
        <translation>状态目录</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="65"/>
        <location filename="../main_window.cpp" line="430"/>
        <location filename="../main_window.hpp" line="75"/>
        <source>Sign in to connect a OneDrive account.</source>
        <translation>登录以连接 OneDrive 账号。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="68"/>
        <location filename="../main_window.cpp" line="276"/>
        <source>Login</source>
        <translation>登录</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="69"/>
        <location filename="../main_window.cpp" line="277"/>
        <source>Browser login</source>
        <translation>浏览器登录</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="71"/>
        <location filename="../main_window.cpp" line="282"/>
        <source>Sign in</source>
        <translation>登录账号</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="73"/>
        <location filename="../main_window.cpp" line="283"/>
        <source>Cancel login</source>
        <translation>取消登录</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="75"/>
        <location filename="../main_window.cpp" line="284"/>
        <source>Open browser</source>
        <translation>打开浏览器</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="87"/>
        <location filename="../main_window.cpp" line="263"/>
        <source>Overview</source>
        <translation>概览</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="103"/>
        <location filename="../main_window.cpp" line="281"/>
        <source>Follow system light/dark appearance</source>
        <translation>跟随系统浅色／深色外观</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="105"/>
        <location filename="../main_window.cpp" line="279"/>
        <source>Appearance</source>
        <translation>外观</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="108"/>
        <location filename="../main_window.cpp" line="285"/>
        <source>Import TOML…</source>
        <translation>导入 TOML…</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="109"/>
        <location filename="../main_window.cpp" line="286"/>
        <source>Save settings</source>
        <translation>保存设置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="124"/>
        <location filename="../main_window.cpp" line="264"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="128"/>
        <location filename="../main_window.cpp" line="300"/>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="179"/>
        <location filename="../main_window.cpp" line="296"/>
        <source>Complete authorization in your system browser.</source>
        <translation>请在系统浏览器中完成授权。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="183"/>
        <source>Waiting for browser authorization (up to 5 minutes)...</source>
        <translation>正在等待浏览器授权（最多 5 分钟）…</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="208"/>
        <source>Login cancelled.</source>
        <translation>登录已取消。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="210"/>
        <source>Login failed: %1</source>
        <translation>登录失败：%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="217"/>
        <source>Signed in. Account credentials saved.</source>
        <translation>已登录。账号凭据已保存。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="227"/>
        <location filename="../main_window.cpp" line="236"/>
        <source>Login completed, but account status could not be refreshed: %1</source>
        <translation>登录已完成，但无法刷新账号状态：%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="305"/>
        <source>System language</source>
        <translation>系统语言</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="308"/>
        <source>English</source>
        <translation>英语</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="311"/>
        <source>Simplified Chinese</source>
        <translation>简体中文</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="337"/>
        <source>Starting browser login...</source>
        <translation>正在启动浏览器登录…</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="345"/>
        <source>Cannot start login: %1</source>
        <translation>无法启动登录：%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="358"/>
        <source>Cancelling login...</source>
        <translation>正在取消登录…</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="366"/>
        <source>Cannot open browser: the login URL is not a valid HTTPS address.</source>
        <translation>无法打开浏览器：登录 URL 不是有效的 HTTPS 地址。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="373"/>
        <source>Cannot open the system browser. Copy the login URL below or try Open browser again.</source>
        <translation>无法打开系统浏览器。请复制下方的登录 URL，或再次点击“打开浏览器”。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="495"/>
        <source>Save these directory changes?

Existing files and synchronization state will not be moved.</source>
        <translation>保存这些目录更改吗？

现有文件和同步状态不会被移动。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="499"/>
        <source>

Sync directory:
%1
→ %2
Inspect a dry run before the next synchronization.</source>
        <translation>

同步目录：
%1
→ %2
下次同步前请检查试运行结果。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="508"/>
        <source>

State directory:
%1
→ %2
Existing account state and credentials will remain in the old location.</source>
        <translation>

状态目录：
%1
→ %2
现有账号状态和凭据将保留在原位置。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="518"/>
        <source>Confirm directory changes</source>
        <translation>确认目录更改</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="533"/>
        <location filename="../main_window.cpp" line="591"/>
        <source>
Previous configuration backed up to:
%1</source>
        <translation>
原配置已备份到：
%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="536"/>
        <source>Settings saved</source>
        <translation>设置已保存</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="480"/>
        <location filename="../main_window.cpp" line="540"/>
        <source>Cannot save settings</source>
        <translation>无法保存设置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="481"/>
        <location filename="../main_window.cpp" line="541"/>
        <source>Settings could not be saved:
%1</source>
        <translation>设置保存失败：
%1</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="531"/>
        <source>Settings saved. Changes take effect on the next operation.</source>
        <translation>设置已保存。更改将在下次操作时生效。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="550"/>
        <location filename="../main_window.cpp" line="579"/>
        <source>Import configuration</source>
        <translation>导入配置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="552"/>
        <source>TOML files (*.toml);;All files (*)</source>
        <translation>TOML 文件 (*.toml);;所有文件 (*)</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="562"/>
        <source>Import this configuration to the active user configuration?

Sync directory: %1
State directory: %2
Sync mode: %3
Deletion policy: %4

Relative file references will be resolved from the active configuration directory. The current configuration will be backed up.</source>
        <translation>将此配置导入到当前用户配置吗？

同步目录：%1
状态目录：%2
同步模式：%3
删除策略：%4

相对文件引用将基于当前配置目录解析。当前配置将被备份。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="589"/>
        <source>Configuration imported.</source>
        <translation>配置已导入。</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="595"/>
        <source>Configuration imported</source>
        <translation>配置已导入</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="600"/>
        <source>Cannot import configuration</source>
        <translation>无法导入配置</translation>
    </message>
    <message>
        <location filename="../main_window.cpp" line="601"/>
        <source>Configuration could not be imported:
%1</source>
        <translation>配置导入失败：
%1</translation>
    </message>
</context>
<context>
    <name>onedrive::gui::SettingsEditor</name>
    <message>
        <location filename="../settings_editor.cpp" line="86"/>
        <source>Sync directory</source>
        <translation>同步目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="98"/>
        <source>State directory</source>
        <translation>状态目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="71"/>
        <source>Application ID</source>
        <translation>应用 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="77"/>
        <source>Tenant ID</source>
        <translation>租户 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="109"/>
        <source>Drive ID</source>
        <translation>驱动器 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="114"/>
        <source>Directory changes do not migrate files, account state or credentials. Settings take effect on the next operation; running operations are not changed.</source>
        <translation>更改目录不会迁移文件、账号状态或凭据。设置将在下次操作时生效，不影响正在运行的操作。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="121"/>
        <source>Advanced account settings</source>
        <translation>高级账号设置</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="125"/>
        <source>Authentication endpoint</source>
        <translation>身份验证端点</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="131"/>
        <source>Graph endpoint</source>
        <translation>Graph 端点</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="137"/>
        <source>Authentication scopes (one per line)</source>
        <translation>身份验证权限范围（每行一个）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="148"/>
        <source>Required mount point (empty: disabled)</source>
        <translation>必需挂载点（留空：禁用）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="162"/>
        <source>Changing authentication endpoints or scopes requires signing in again. Use only trusted cloud endpoints; tokens may be sent to the Graph endpoint. A mount point is a preflight requirement, not a directory migration.</source>
        <translation>更改身份验证端点或权限范围后需要重新登录。仅使用可信的云端点；令牌可能会发送至 Graph 端点。挂载点是预检要求，不会迁移目录。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="176"/>
        <source>Sync mode</source>
        <translation>同步模式</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="188"/>
        <source>Bidirectional</source>
        <translation>双向同步</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="189"/>
        <source>Upload only</source>
        <translation>仅上传</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="190"/>
        <source>Download only</source>
        <translation>仅下载</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="194"/>
        <source>Deletion policy</source>
        <translation>删除策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="203"/>
        <source>Propagate deletions</source>
        <translation>传播删除操作</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="204"/>
        <source>Preserve files</source>
        <translation>保留文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="208"/>
        <source>Local conflict policy</source>
        <translation>本地冲突策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="217"/>
        <source>Block</source>
        <translation>阻止</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="218"/>
        <source>Back up conflicts</source>
        <translation>备份冲突文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="222"/>
        <source>Selective sync rules file (empty: disabled)</source>
        <translation>选择性同步规则文件（留空：禁用）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="231"/>
        <source>Dotfiles</source>
        <translation>点文件策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="239"/>
        <source>Include</source>
        <translation>包含</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="240"/>
        <source>Exclude</source>
        <translation>排除</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="244"/>
        <source>Maximum file size (bytes; 0: unlimited)</source>
        <translation>最大文件大小（字节；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="249"/>
        <source>Advanced sync rules</source>
        <translation>高级同步规则</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="253"/>
        <source>Honor .nosync files</source>
        <translation>遵循 .nosync 文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="261"/>
        <source>Include root files with selective sync</source>
        <translation>选择性同步时包含根目录文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="269"/>
        <source>Maximum remote deletions (0: no deletions)</source>
        <translation>最大远程删除数量（0：禁止删除）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="279"/>
        <source>Upload concurrency (1–16)</source>
        <translation>上传并发数（1–16）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="285"/>
        <source>Download concurrency (1–16)</source>
        <translation>下载并发数（1–16）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="291"/>
        <source>Upload rate per request (bytes/s; 0: unlimited)</source>
        <translation>单请求上传速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="299"/>
        <source>Download rate per request (bytes/s; 0: unlimited)</source>
        <translation>单请求下载速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="307"/>
        <source>Aggregate upload rate (bytes/s; 0: unlimited)</source>
        <translation>总上传速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="317"/>
        <source>Aggregate download rate (bytes/s; 0: unlimited)</source>
        <translation>总下载速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="327"/>
        <source>Transfer order</source>
        <translation>传输顺序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="343"/>
        <source>Default</source>
        <translation>默认</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="344"/>
        <source>Smallest first</source>
        <translation>小文件优先</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="345"/>
        <source>Largest first</source>
        <translation>大文件优先</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="346"/>
        <source>Name ascending</source>
        <translation>名称升序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="347"/>
        <source>Name descending</source>
        <translation>名称降序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="349"/>
        <source>Advanced transfer settings</source>
        <translation>高级传输设置</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="353"/>
        <source>Upload chunk (bytes; multiple of 327680, less than 62914560)</source>
        <translation>上传分块大小（字节；327680 的倍数，小于 62914560）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="363"/>
        <source>Download chunk threshold (bytes; greater than 0)</source>
        <translation>下载分块阈值（字节；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="371"/>
        <source>Download checkpoint interval (bytes; greater than 0)</source>
        <translation>下载检查点间隔（字节；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="379"/>
        <source>Maximum download retries</source>
        <translation>最大下载重试次数</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="387"/>
        <source>Download validation</source>
        <translation>下载校验</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="396"/>
        <source>Strict</source>
        <translation>严格</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="396"/>
        <source>Relaxed</source>
        <translation>宽松</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="403"/>
        <source>Proxy URL (empty override: direct connection)</source>
        <translation>代理 URL（空覆盖值：直接连接）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="417"/>
        <source>Proxy bypass hosts (one per line; empty override: none)</source>
        <translation>绕过代理的主机（每行一个；空覆盖值：无）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="435"/>
        <source>Proxy username (empty: unset)</source>
        <translation>代理用户名（留空：未设置）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="444"/>
        <source>Proxy password file (empty: unset)</source>
        <translation>代理密码文件（留空：未设置）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="454"/>
        <source>Proxy authentication</source>
        <translation>代理身份验证</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="470"/>
        <location filename="../settings_editor.cpp" line="556"/>
        <location filename="../settings_editor.cpp" line="574"/>
        <location filename="../settings_editor.cpp" line="735"/>
        <source>Automatic</source>
        <translation>自动</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="471"/>
        <source>Basic</source>
        <translation>基本身份验证</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="472"/>
        <source>Digest</source>
        <translation>摘要身份验证</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="473"/>
        <source>NTLM</source>
        <translation>NTLM</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="474"/>
        <source>Negotiate</source>
        <translation>协商身份验证</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="478"/>
        <source>HTTPS proxy CA file (empty: unset)</source>
        <translation>HTTPS 代理 CA 文件（留空：未设置）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="487"/>
        <source>Unchecked overrides inherit proxy environment variables independently. To inherit again, uncheck both overrides and clear credentials and CA settings. Never put passwords in the URL: use a private password file owned by you. Files are referenced, not copied or read by this editor; runtime security checks still apply. A password file requires a username, and a CA file requires an HTTPS proxy.</source>
        <translation>未勾选的覆盖项分别继承代理环境变量。若要恢复继承，请取消两项覆盖并清除凭据及 CA 设置。切勿在 URL 中填写密码：请使用归您所有的私有密码文件。编辑器仅引用文件，不复制或读取内容；运行时仍执行安全检查。密码文件需要用户名，CA 文件需要 HTTPS 代理。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="504"/>
        <source>Connection timeout (seconds; greater than 0)</source>
        <translation>连接超时（秒；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="514"/>
        <source>Request timeout (seconds; greater than 0)</source>
        <translation>请求超时（秒；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="524"/>
        <source>Stall timeout (seconds; 0: disabled)</source>
        <translation>停滞超时（秒；0：禁用）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="534"/>
        <source>Stall minimum rate (bytes/s; greater than 0)</source>
        <translation>停滞最低速率（字节/秒；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="544"/>
        <source>HTTP version</source>
        <translation>HTTP 版本</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="557"/>
        <source>HTTP/1.1</source>
        <translation>HTTP/1.1</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="558"/>
        <source>HTTP/2</source>
        <translation>HTTP/2</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="562"/>
        <source>IP version</source>
        <translation>IP 版本</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="575"/>
        <source>IPv4</source>
        <translation>IPv4</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="576"/>
        <source>IPv6</source>
        <translation>IPv6</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="577"/>
        <source>Graph throttling</source>
        <translation>Graph 限流</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="581"/>
        <source>Maximum throttle retries (0: no retries)</source>
        <translation>限流最大重试次数（0：不重试）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="589"/>
        <source>Initial throttle delay (seconds)</source>
        <translation>限流初始延迟（秒）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="597"/>
        <source>Maximum throttle delay (seconds; at least initial delay)</source>
        <translation>限流最大延迟（秒；不小于初始延迟）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="607"/>
        <source>Enable WebSocket notifications</source>
        <translation>启用 WebSocket 通知</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="615"/>
        <source>Poll interval (seconds; 1–86400)</source>
        <translation>轮询间隔（秒；1–86400）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="623"/>
        <source>Local settle delay (milliseconds; 0–60000)</source>
        <translation>本地变更稳定等待时间（毫秒；0–60000）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="629"/>
        <source>Advanced WebSocket settings</source>
        <translation>高级 WebSocket 设置</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="633"/>
        <source>WebSocket request timeout (seconds; 1–300)</source>
        <translation>WebSocket 请求超时（秒；1–300）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="641"/>
        <source>WebSocket connection timeout (seconds; 1–300)</source>
        <translation>WebSocket 连接超时（秒；1–300）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="649"/>
        <source>WebSocket renewal lead (seconds; 0–3600)</source>
        <translation>WebSocket 提前续订时间（秒；0–3600）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="657"/>
        <source>WebSocket initial backoff (seconds; 1–3600)</source>
        <translation>WebSocket 初始退避时间（秒；1–3600）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="665"/>
        <source>WebSocket maximum backoff (seconds; 1–86400; at least initial backoff)</source>
        <translation>WebSocket 最大退避时间（秒；1–86400；不小于初始退避）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="675"/>
        <source>These settings configure the next CLI or service monitor operation. This GUI does not yet start synchronization or monitoring workers.</source>
        <translation>这些设置用于下次 CLI 或服务监控操作。此 GUI 尚不启动同步或监控任务。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="686"/>
        <source>Logging level</source>
        <translation>日志级别</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="689"/>
        <source>Trace</source>
        <translation>跟踪</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="690"/>
        <source>Debug</source>
        <translation>调试</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="691"/>
        <source>Info</source>
        <translation>信息</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="692"/>
        <source>Warning</source>
        <translation>警告</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="693"/>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="694"/>
        <source>Critical</source>
        <translation>严重错误</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="695"/>
        <source>Off</source>
        <translation>关闭</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="699"/>
        <source>Log file (empty: unset)</source>
        <translation>日志文件（留空：未设置）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="709"/>
        <source>Sync file permissions</source>
        <translation>同步文件权限</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="718"/>
        <source>Private (owner only)</source>
        <translation>私有（仅所有者）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="719"/>
        <source>Use process umask</source>
        <translation>使用进程 umask</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="723"/>
        <source>Filesystem metadata</source>
        <translation>文件系统元数据</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="736"/>
        <source>Extended attributes</source>
        <translation>扩展属性</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="737"/>
        <source>Database</source>
        <translation>数据库</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="740"/>
        <source>The active configuration path is shown above. For diagnostics, use the CLI doctor command and check the configured log file. Debug and trace logs may contain private metadata; review them before sharing. Permission changes affect newly created files, not existing files.</source>
        <translation>当前配置路径显示在上方。诊断时请使用 CLI doctor 命令并检查配置的日志文件。调试和跟踪日志可能包含私有元数据，分享前请检查。权限变更影响新建文件，不改变已有文件。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="791"/>
        <location filename="../settings_editor.cpp" line="905"/>
        <source>Browse…</source>
        <translation>浏览…</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="799"/>
        <source>Select directory</source>
        <translation>选择目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="802"/>
        <source>Select file</source>
        <translation>选择文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="814"/>
        <location filename="../settings_editor.cpp" line="887"/>
        <source>Override environment</source>
        <translation>覆盖环境变量</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="881"/>
        <source>Network</source>
        <translation>网络</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="882"/>
        <source>Monitor</source>
        <translation>监控</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="883"/>
        <source>Advanced and logs</source>
        <translation>高级与日志</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="878"/>
        <source>Account and directories</source>
        <translation>账号与目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="879"/>
        <source>Sync rules</source>
        <translation>同步规则</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="880"/>
        <source>Transfers</source>
        <translation>传输</translation>
    </message>
</context>
</TS>
