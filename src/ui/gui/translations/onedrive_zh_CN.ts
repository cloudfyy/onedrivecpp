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
        <location filename="../settings_editor.cpp" line="84"/>
        <source>Sync directory</source>
        <translation>同步目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="96"/>
        <source>State directory</source>
        <translation>状态目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="69"/>
        <source>Application ID</source>
        <translation>应用 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="75"/>
        <source>Tenant ID</source>
        <translation>租户 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="107"/>
        <source>Drive ID</source>
        <translation>驱动器 ID</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="112"/>
        <source>Directory changes do not migrate files, account state or credentials. Settings take effect on the next operation; running operations are not changed.</source>
        <translation>更改目录不会迁移文件、账号状态或凭据。设置将在下次操作时生效，不影响正在运行的操作。</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="123"/>
        <source>Sync mode</source>
        <translation>同步模式</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="135"/>
        <source>Bidirectional</source>
        <translation>双向同步</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="136"/>
        <source>Upload only</source>
        <translation>仅上传</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="137"/>
        <source>Download only</source>
        <translation>仅下载</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="141"/>
        <source>Deletion policy</source>
        <translation>删除策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="150"/>
        <source>Propagate deletions</source>
        <translation>传播删除操作</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="151"/>
        <source>Preserve files</source>
        <translation>保留文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="155"/>
        <source>Local conflict policy</source>
        <translation>本地冲突策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="164"/>
        <source>Block</source>
        <translation>阻止</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="165"/>
        <source>Back up conflicts</source>
        <translation>备份冲突文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="169"/>
        <source>Selective sync rules file (empty: disabled)</source>
        <translation>选择性同步规则文件（留空：禁用）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="178"/>
        <source>Dotfiles</source>
        <translation>点文件策略</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="186"/>
        <source>Include</source>
        <translation>包含</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="187"/>
        <source>Exclude</source>
        <translation>排除</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="191"/>
        <source>Maximum file size (bytes; 0: unlimited)</source>
        <translation>最大文件大小（字节；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="196"/>
        <source>Advanced sync rules</source>
        <translation>高级同步规则</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="200"/>
        <source>Honor .nosync files</source>
        <translation>遵循 .nosync 文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="208"/>
        <source>Include root files with selective sync</source>
        <translation>选择性同步时包含根目录文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="216"/>
        <source>Maximum remote deletions (0: no deletions)</source>
        <translation>最大远程删除数量（0：禁止删除）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="226"/>
        <source>Upload concurrency (1–16)</source>
        <translation>上传并发数（1–16）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="232"/>
        <source>Download concurrency (1–16)</source>
        <translation>下载并发数（1–16）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="238"/>
        <source>Upload rate per request (bytes/s; 0: unlimited)</source>
        <translation>单请求上传速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="246"/>
        <source>Download rate per request (bytes/s; 0: unlimited)</source>
        <translation>单请求下载速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="254"/>
        <source>Aggregate upload rate (bytes/s; 0: unlimited)</source>
        <translation>总上传速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="264"/>
        <source>Aggregate download rate (bytes/s; 0: unlimited)</source>
        <translation>总下载速率（字节/秒；0：不限）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="274"/>
        <source>Transfer order</source>
        <translation>传输顺序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="290"/>
        <source>Default</source>
        <translation>默认</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="291"/>
        <source>Smallest first</source>
        <translation>小文件优先</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="292"/>
        <source>Largest first</source>
        <translation>大文件优先</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="293"/>
        <source>Name ascending</source>
        <translation>名称升序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="294"/>
        <source>Name descending</source>
        <translation>名称降序</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="296"/>
        <source>Advanced transfer settings</source>
        <translation>高级传输设置</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="300"/>
        <source>Upload chunk (bytes; multiple of 327680, less than 62914560)</source>
        <translation>上传分块大小（字节；327680 的倍数，小于 62914560）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="310"/>
        <source>Download chunk threshold (bytes; greater than 0)</source>
        <translation>下载分块阈值（字节；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="318"/>
        <source>Download checkpoint interval (bytes; greater than 0)</source>
        <translation>下载检查点间隔（字节；大于 0）</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="326"/>
        <source>Maximum download retries</source>
        <translation>最大下载重试次数</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="334"/>
        <source>Download validation</source>
        <translation>下载校验</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="343"/>
        <source>Strict</source>
        <translation>严格</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="343"/>
        <source>Relaxed</source>
        <translation>宽松</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="381"/>
        <location filename="../settings_editor.cpp" line="461"/>
        <source>Browse…</source>
        <translation>浏览…</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="389"/>
        <source>Select directory</source>
        <translation>选择目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="392"/>
        <source>Select rules file</source>
        <translation>选择规则文件</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="440"/>
        <source>Account and directories</source>
        <translation>账号与目录</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="441"/>
        <source>Sync rules</source>
        <translation>同步规则</translation>
    </message>
    <message>
        <location filename="../settings_editor.cpp" line="442"/>
        <source>Transfers</source>
        <translation>传输</translation>
    </message>
</context>
</TS>
