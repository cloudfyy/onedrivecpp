#include "ui/cli/localization.hpp"

#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace onedrive::cli::detail {
namespace {

struct TranslationEntry {
    std::string_view source;
    std::string_view target;
};

[[nodiscard]] std::optional<std::string_view> lookup_entry(
    const auto& table, std::string_view key
) noexcept {
    for (const auto& entry : table) {
        if (entry.source == key) {
            return entry.target;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>>
extract_between(
    std::string_view text,
    std::string_view prefix,
    std::string_view middle,
    std::string_view suffix
) noexcept {
    if (!text.starts_with(prefix) || !text.ends_with(suffix) ||
        text.size() < prefix.size() + middle.size() + suffix.size()) {
        return std::nullopt;
    }
    const auto body = text.substr(
        prefix.size(), text.size() - prefix.size() - suffix.size()
    );
    const auto split = body.find(middle);
    if (split == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{
        body.substr(0, split), body.substr(split + middle.size())
    };
}

[[nodiscard]] std::optional<std::string_view> extract_single(
    std::string_view text, std::string_view prefix, std::string_view suffix
) noexcept {
    if (!text.starts_with(prefix) || !text.ends_with(suffix) ||
        text.size() < prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    return text.substr(
        prefix.size(), text.size() - prefix.size() - suffix.size()
    );
}

constexpr std::array kTextTranslations{
    // CLI11 help descriptions and group labels
    TranslationEntry{
        "A modern C++ OneDrive synchronization client",
        "现代 C++ OneDrive 同步客户端",
    },
    TranslationEntry{"Usage", "用法"},
    TranslationEntry{"Options", "选项"},
    TranslationEntry{"Subcommands", "子命令"},
    TranslationEntry{"Commands", "命令"},
    TranslationEntry{"Positionals", "位置参数"},
    TranslationEntry{"Print this help message and exit", "显示帮助信息并退出"},
    TranslationEntry{
        "Display program version information and exit", "显示版本信息并退出"
    },
    TranslationEntry{
        "Manage Microsoft account authentication", "管理 Microsoft 账户登录认证"
    },
    TranslationEntry{
        "Authorize with Microsoft using the device code flow",
        "使用浏览器或设备码流程完成 Microsoft 登录授权",
    },
    TranslationEntry{
        "Remove the locally stored refresh token", "清除本地保存的刷新令牌"
    },
    TranslationEntry{
        "Inspect account, cloud, and synchronization state",
        "查看账户、云端及本地同步状态",
    },
    TranslationEntry{
        "Run local synchronization-state diagnostics",
        "运行本地同步状态数据库诊断",
    },
    TranslationEntry{
        "List OneDrive drives available to the active account",
        "列出当前账户可用的 OneDrive 驱动器",
    },
    TranslationEntry{
        "List items shared with the account and OneDrive shortcuts",
        "列出与当前账户共享的项目及 OneDrive 快捷方式",
    },
    TranslationEntry{
        "Find SharePoint sites and their document libraries",
        "查找 SharePoint 站点及其文档库",
    },
    TranslationEntry{
        "Show storage quota for the configured drive",
        "显示所配置驱动器的存储配额",
    },
    TranslationEntry{
        "Show read-only synchronization status", "显示只读同步状态摘要"
    },
    TranslationEntry{
        "List saved blocked items without connecting to OneDrive",
        "列出已记录的受阻项目（无需连接 OneDrive）",
    },
    TranslationEntry{
        "Show local synchronization storage usage", "显示本地同步目录存储占用"
    },
    TranslationEntry{
        "List resumable and invalid partial downloads",
        "列出可续传及无效的分片下载记录",
    },
    TranslationEntry{
        "Check downloaded files against saved synchronization state",
        "对比已保存的同步状态检查已下载文件",
    },
    TranslationEntry{
        "Verify downloaded file metadata or content",
        "校验已下载文件的元数据或内容哈希",
    },
    TranslationEntry{
        "Show effective non-secret configuration", "显示脱敏后的有效配置"
    },
    TranslationEntry{
        "Maintain saved synchronization state", "维护已保存的本地同步状态"
    },
    TranslationEntry{
        "Reset the cloud change cursor while preserving item state",
        "重置云端增量游标并保留已跟踪项目状态",
    },
    TranslationEntry{
        "Remove invalid and orphaned partial download state",
        "清理无效及孤立的分片下载临时文件与记录",
    },
    TranslationEntry{
        "Back up and upgrade local state databases without syncing",
        "备份并升级本地状态数据库（不执行同步）",
    },
    TranslationEntry{
        "Clear all saved synchronization state for the configured drive",
        "清除所配置驱动器的全部已保存同步状态",
    },
    TranslationEntry{"Transfer OneDrive files", "同步、下载或恢复 OneDrive 文件"},
    TranslationEntry{"Synchronize OneDrive files", "同步 OneDrive 文件"},
    TranslationEntry{
        "Download one remote file by its Drive-relative path",
        "按驱动器相对路径下载单个远端文件",
    },
    TranslationEntry{
        "Restore missing local files without changing cloud content",
        "恢复缺失的本地文件（不修改云端内容）",
    },
    TranslationEntry{
        "Watch for synchronization changes", "持续监控本地与云端更改并自动同步"
    },
    TranslationEntry{
        "Explicitly control the running local Monitor",
        "显式控制正在运行的本地监控进程",
    },
    TranslationEntry{
        "Query the running Monitor without connecting to OneDrive",
        "查询正在运行的监控进程状态（无需连接 OneDrive）",
    },
    TranslationEntry{
        "Request a synchronization check (acknowledgement, not completion)",
        "请求立即触发一次同步检查（仅确认收到请求）",
    },
    TranslationEntry{
        "Request cancellation and shutdown (acknowledgement, not completion)",
        "请求取消并停止监控进程（仅确认收到请求）",
    },
    TranslationEntry{"Path to the configuration file", "配置文件路径"},
    TranslationEntry{
        "Minimum log level (overrides configuration)",
        "最低日志级别（覆盖配置文件设置）",
    },
    TranslationEntry{
        "Write rotating logs to this file (overrides configuration)",
        "将轮转日志写入此文件（覆盖配置文件设置）",
    },
    TranslationEntry{
        "Color output override: auto, always, or never",
        "颜色输出模式：auto、always 或 never",
    },
    TranslationEntry{
        "Output format: text or json", "输出格式：text 或 json"
    },
    TranslationEntry{
        "Display language: auto, en, or zh_CN",
        "界面显示语言：auto、en 或 zh_CN",
    },
    TranslationEntry{
        "Suppress informational and success output",
        "静默模式，隐藏普通信息与成功提示",
    },
    TranslationEntry{
        "Authentication flow: auto, auth-code, or device-code",
        "认证流程模式：auto、auth-code 或 device-code",
    },
    TranslationEntry{
        "Confirm the state clear without an interactive prompt",
        "跳过交互式确认提示，直接确认清除状态",
    },
    TranslationEntry{
        "Show cleanup candidates without removing files or state",
        "仅显示待清理候选项目，不删除文件或状态记录",
    },
    TranslationEntry{
        "Confirm removal of invalid partial files and state",
        "跳过交互式确认提示，确认清理无效分片文件与记录",
    },
    TranslationEntry{
        "List database versions and upgrade candidates without changing them",
        "仅列出数据库版本及待升级项，不实际修改",
    },
    TranslationEntry{
        "Confirm database backups and upgrades",
        "跳过交互式确认提示，确认备份并升级数据库",
    },
    TranslationEntry{
        "Show synchronization inputs without changing remote files",
        "仅预览同步计划，不修改本地或远端文件",
    },
    TranslationEntry{
        "Allow this sync to exceed the configured remote deletion limit",
        "允许本次同步超出所配置的最大远端删除数量限制",
    },
    TranslationEntry{
        "Interface: auto, console, or tui", "界面模式：auto、console 或 tui"
    },
    TranslationEntry{
        "TUI theme: hacker, ocean, amber, or synthwave",
        "TUI 颜色主题：hacker、ocean、amber 或 synthwave",
    },
    TranslationEntry{
        "Drive-relative path of the remote file", "远端文件的驱动器相对路径"
    },
    TranslationEntry{
        "Show the single-file download plan without changing local state",
        "仅显示单文件下载计划，不修改本地状态",
    },
    TranslationEntry{
        "Optional Drive-relative file or directory; omit for the sync scope",
        "可选的驱动器相对文件或目录路径；省略则恢复整个同步范围",
    },
    TranslationEntry{
        "List missing local files without downloading or changing sync state",
        "仅列出缺失的本地文件，不执行下载或修改同步状态",
    },
    TranslationEntry{
        "Optional SharePoint site search query, URL, or hostname:/sites/path",
        "可选的 SharePoint 站点搜索词、URL 或 hostname:/sites/path",
    },
    TranslationEntry{
        "Only show this exact reason code", "仅显示匹配此原因代码的受阻项"
    },
    TranslationEntry{
        "Search paths, reason codes, and messages (ASCII case-insensitive)",
        "搜索路径、原因代码及详情消息（不区分 ASCII 大小写）",
    },
    TranslationEntry{
        "Optional Drive-relative file or directory path",
        "可选的驱动器相对文件或目录路径",
    },
    TranslationEntry{
        "Only show files with this status", "仅显示指定状态的文件"
    },
    TranslationEntry{
        "Verification mode: metadata or content",
        "校验模式：metadata（元数据）或 content（内容哈希）",
    },

    // Browser auth callback messages
    TranslationEntry{
        "Authorization received. Return to OneDrive C++. You may close this tab.",
        "已收到授权响应。请返回 OneDrive C++，您可以关闭此标签页。",
    },
    TranslationEntry{
        "Invalid authorization callback.", "无效的授权回调请求。"
    },

    // TUI chrome & labels
    TranslationEntry{"DOWNLOADS", "下载"},
    TranslationEntry{"UPLOADS", "上传"},
    TranslationEntry{"MONITORING", "实时监控"},
    TranslationEntry{"SYNC", "同步"},
    TranslationEntry{
        "Totals unknown; percentage unavailable.",
        "总量未知，暂无法计算百分比。",
    },
    TranslationEntry{"calculating...", "正在计算..."},
    TranslationEntry{"  PATH", "  路径"},
    TranslationEntry{
        "STATUS  SAVED PROGRESS / TOTAL", "状态  已保存进度 / 总大小"
    },
    TranslationEntry{
        "Last synchronization round incomplete.", "上一轮同步未完全完成。"
    },
    TranslationEntry{"ACTIVITY", "活动动态"},
    TranslationEntry{" q / Esc  EXIT ", " q / Esc  退出 "},
    TranslationEntry{
        " Up/Down or k/j: select file ", " 上下方向键 或 k/j：选择文件 "
    },
    TranslationEntry{
        " Left/Right or n/p: previous/next Drive ",
        " 左右方向键 或 n/p：上一个/下一个驱动器 ",
    },
    TranslationEntry{
        " Left/Right or n/p: previous/next item ",
        " 左右方向键 或 n/p：上一项/下一项 ",
    },
    TranslationEntry{
        " Press Enter to exit (or q) ", " 按 Enter 退出（或按 q） "
    },
};

constexpr std::array kSectionTitlesByEvent{
    TranslationEntry{"account", "Microsoft 账户："},
    TranslationEntry{"device_code", "需要使用设备码登录："},
    TranslationEntry{"browser_auth", "需要使用浏览器登录："},
    TranslationEntry{"drive_target", "配置的驱动器："},
    TranslationEntry{"configured_drive", "配置的同步驱动器："},
    TranslationEntry{"drive", "OneDrive 驱动器："},
    TranslationEntry{"shared_resource", "共享的 OneDrive 资源："},
    TranslationEntry{"shared_item", "与我共享的项目："},
    TranslationEntry{"sharepoint_site", "SharePoint 站点："},
    TranslationEntry{"site_drive", "SharePoint 文档库："},
    TranslationEntry{"sharepoint_drive", "SharePoint 文档库："},
    TranslationEntry{"quota", "OneDrive 存储配额："},
    TranslationEntry{"status", "同步状态："},
    TranslationEntry{"sync_status", "同步状态："},
    TranslationEntry{"blocked_item", "受阻项目："},
    TranslationEntry{"blocked_item_summary", "受阻项目摘要："},
    TranslationEntry{"database_integrity", "同步状态数据库："},
    TranslationEntry{"database_migrated", "数据库升级已完成："},
    TranslationEntry{"health_check", "健康检查："},
    TranslationEntry{"effective_config", "有效配置："},
    TranslationEntry{"effective_auth_config", "有效认证配置："},
    TranslationEntry{"effective_graph_config", "有效 Graph 配置："},
    TranslationEntry{"effective_sync_config", "有效同步配置："},
    TranslationEntry{"effective_transfer_config", "有效传输配置："},
    TranslationEntry{"effective_monitor_config", "有效监控配置："},
    TranslationEntry{"effective_output_config", "有效输出配置："},
    TranslationEntry{"storage", "本地同步存储占用："},
    TranslationEntry{"partial_download", "分片下载："},
    TranslationEntry{"partial_download_summary", "分片下载摘要："},
    TranslationEntry{"downloaded_file", "已下载文件："},
    TranslationEntry{"downloaded_file_summary", "已下载文件摘要："},
    TranslationEntry{"file_verification", "文件校验："},
    TranslationEntry{"file_verification_summary", "文件校验摘要："},
    TranslationEntry{"cleanup_partial", "分片清理候选项："},
    TranslationEntry{"cleanup_orphan", "分片清理候选项："},
    TranslationEntry{"cleanup_summary", "分片清理摘要："},
    TranslationEntry{"cleanup_result", "分片清理结果："},
    TranslationEntry{"monitor_control", "本地监控控制："},
    TranslationEntry{"large_delete_guard", "大批量远端删除保护："},
    TranslationEntry{"single_file_download_plan", "单文件下载计划："},
    TranslationEntry{"restore_summary", "缺失文件恢复摘要："},
};

constexpr std::array kFieldLabelsByKey{
    TranslationEntry{"verification_uri", "验证网址："},
    TranslationEntry{"user_code", "设备验证码："},
    TranslationEntry{"authorize_url", "授权链接："},
    TranslationEntry{"redirect_uri", "回调地址："},
    TranslationEntry{"drive_reference", "驱动器："},
    TranslationEntry{"reference", "配置引用："},
    TranslationEntry{"configured", "已配置："},
    TranslationEntry{"site", "站点："},
    TranslationEntry{"site_id", "站点 ID："},
    TranslationEntry{"site_web_url", "站点链接："},
    TranslationEntry{"source", "来源："},
    TranslationEntry{"name", "名称："},
    TranslationEntry{"target_name", "目标名称："},
    TranslationEntry{"kind", "类型："},
    TranslationEntry{"id", "ID："},
    TranslationEntry{"drive", "驱动器："},
    TranslationEntry{"drive_id", "驱动器 ID："},
    TranslationEntry{"drive_type", "类型："},
    TranslationEntry{"item_id", "项目 ID："},
    TranslationEntry{"remote_id", "项目 ID："},
    TranslationEntry{"remote_drive_id", "远端驱动器 ID："},
    TranslationEntry{"remote_item_id", "远端项目 ID："},
    TranslationEntry{"type", "类型："},
    TranslationEntry{"owner", "所有者："},
    TranslationEntry{"shared_by", "共享者："},
    TranslationEntry{"size", "大小："},
    TranslationEntry{"web_url", "网页链接："},
    TranslationEntry{"total", "总容量："},
    TranslationEntry{"used", "已用空间："},
    TranslationEntry{"remaining", "剩余空间："},
    TranslationEntry{"deleted", "回收站占用："},
    TranslationEntry{"state", "状态："},
    TranslationEntry{"authenticated", "已认证："},
    TranslationEntry{"account", "账户："},
    TranslationEntry{"token_file", "凭据文件："},
    TranslationEntry{"state_database", "状态数据库："},
    TranslationEntry{"sync_directory", "同步目录："},
    TranslationEntry{"data_directory", "数据目录："},
    TranslationEntry{"data_mount_point", "数据挂载点："},
    TranslationEntry{"local_path", "本地路径："},
    TranslationEntry{"remote_path", "远端路径："},
    TranslationEntry{"destination", "目标路径："},
    TranslationEntry{"temporary_path", "临时文件："},
    TranslationEntry{"path", "路径："},
    TranslationEntry{"statistics_source", "统计来源："},
    TranslationEntry{"known_files", "已知文件数："},
    TranslationEntry{"downloaded_files", "已下载（本地）："},
    TranslationEntry{"pending_files", "待下载文件数："},
    TranslationEntry{"blocked_files", "受阻文件数："},
    TranslationEntry{"last_sync", "上次同步时间："},
    TranslationEntry{"last_result", "上次结果："},
    TranslationEntry{"last_result_name", "上次结果名称："},
    TranslationEntry{"tracked_items", "受跟踪项目："},
    TranslationEntry{"tracked_files", "受跟踪文件："},
    TranslationEntry{"tracked_directories", "受跟踪目录："},
    TranslationEntry{"tracked_bytes", "受跟踪字节："},
    TranslationEntry{"blocked_items", "受阻项目："},
    TranslationEntry{"pending_downloads", "待下载项："},
    TranslationEntry{"partial_downloads", "分片下载数："},
    TranslationEntry{"resumable_partials", "可续传分片："},
    TranslationEntry{"partial_bytes", "分片字节："},
    TranslationEntry{"pending_uploads", "待上传项："},
    TranslationEntry{"pending_deletes", "待删除项："},
    TranslationEntry{"pending_remote_moves", "待执行远端移动："},
    TranslationEntry{"pending_local_moves", "待执行本地移动："},
    TranslationEntry{"pending_delta_batches", "待处理增量批次："},
    TranslationEntry{"pending_delta_changes", "待处理增量变更："},
    TranslationEntry{"delta_cursor", "增量游标："},
    TranslationEntry{"sync_filter_fingerprint", "过滤规则指纹："},
    TranslationEntry{"reason_code", "原因代码："},
    TranslationEntry{"reason_message", "详情："},
    TranslationEntry{"reason", "原因："},
    TranslationEntry{"detail", "详情："},
    TranslationEntry{"attempt_count", "记录发现次数："},
    TranslationEntry{"policy_control", "策略控制："},
    TranslationEntry{"recovery_allowed", "允许自动恢复："},
    TranslationEntry{"root", "根目录："},
    TranslationEntry{"root_exists", "根目录存在："},
    TranslationEntry{"filesystem_capacity", "文件系统总容量："},
    TranslationEntry{"filesystem_available", "文件系统可用空间："},
    TranslationEntry{"config_file", "配置文件："},
    TranslationEntry{"state_directory", "状态目录："},
    TranslationEntry{"sync_mount_point", "同步挂载点："},
    TranslationEntry{"sync_list", "选择性同步规则："},
    TranslationEntry{"sync_mode", "同步模式："},
    TranslationEntry{"mode", "模式："},
    TranslationEntry{"delete_policy", "删除策略："},
    TranslationEntry{"maximum_remote_deletions", "配置上限："},
    TranslationEntry{"dry_run", "仅预览（dry-run）："},
    TranslationEntry{"order", "传输顺序："},
    TranslationEntry{"download_concurrency", "下载并发数："},
    TranslationEntry{"download_maximum_retries", "下载重试次数："},
    TranslationEntry{"download_rate", "单文件下载限速："},
    TranslationEntry{"download_total_rate", "总下载限速："},
    TranslationEntry{"upload_concurrency", "上传并发数："},
    TranslationEntry{"upload_rate", "单文件上传限速："},
    TranslationEntry{"upload_total_rate", "总上传限速："},
    TranslationEntry{"websocket_enabled", "WebSocket："},
    TranslationEntry{"log_level", "日志级别："},
    TranslationEntry{"log_file", "日志文件："},
    TranslationEntry{"output", "输出模式："},
    TranslationEntry{"color", "颜色模式："},
    TranslationEntry{"ui", "界面模式："},
    TranslationEntry{"theme", "主题："},
    TranslationEntry{"language", "语言："},
    TranslationEntry{"file_history_limit", "文件历史记录上限："},
    TranslationEntry{"proxy", "代理："},
    TranslationEntry{"completed_bytes", "已保存下载进度："},
    TranslationEntry{"expected_bytes", "文件总大小："},
    TranslationEntry{"actual_bytes", "本地临时文件大小："},
    TranslationEntry{"status", "状态："},
    TranslationEntry{"recorded", "已记录："},
    TranslationEntry{"resumable", "可续传："},
    TranslationEntry{"invalid", "无效："},
    TranslationEntry{"invalid_records", "无效记录数："},
    TranslationEntry{"orphaned_files", "孤立文件数："},
    TranslationEntry{"retained", "保留项数："},
    TranslationEntry{"record_only", "仅清理记录："},
    TranslationEntry{"planned_files", "待移除文件数："},
    TranslationEntry{"planned_bytes", "待移除文件字节："},
    TranslationEntry{"skipped", "已跳过："},
    TranslationEntry{"failed", "失败："},
    TranslationEntry{"action", "操作："},
    TranslationEntry{"bytes", "文件大小（字节）："},
    TranslationEntry{"removed_records", "已移除记录数："},
    TranslationEntry{"removed_files", "已移除文件数："},
    TranslationEntry{"removed_bytes", "已移除文件字节："},
    TranslationEntry{"expected_size", "预期大小："},
    TranslationEntry{"actual_size", "实际大小："},
    TranslationEntry{"inspected", "已检查："},
    TranslationEntry{"shown", "已显示："},
    TranslationEntry{"ok", "正常："},
    TranslationEntry{"missing", "缺失："},
    TranslationEntry{"modified", "已修改："},
    TranslationEntry{"type_changed", "类型已变更："},
    TranslationEntry{"outside_root", "超出同步根目录："},
    TranslationEntry{"result", "校验结果："},
    TranslationEntry{"algorithm", "算法："},
    TranslationEntry{"verified", "已通过："},
    TranslationEntry{"hash_unavailable", "哈希不可用："},
    TranslationEntry{"schema_version", "模式版本："},
    TranslationEntry{"previous_schema_version", "升级前版本："},
    TranslationEntry{"latest_schema_version", "最新版本："},
    TranslationEntry{"backup_path", "备份路径："},
    TranslationEntry{"request", "请求："},
    TranslationEntry{"acknowledgement", "确认响应："},
    TranslationEntry{"pid", "进程 PID："},
    TranslationEntry{"instance_id", "实例 ID："},
    TranslationEntry{"pending", "待处理："},
    TranslationEntry{"completed_rounds", "已完成轮数："},
    TranslationEntry{"delete_operations", "Graph 删除操作数："},
    TranslationEntry{"affected_items", "受影响跟踪项数："},
    TranslationEntry{"forced", "强制覆盖："},
};

constexpr std::array kFieldLabelsByText{
    TranslationEntry{"temporary files total:", "临时文件总大小："},
    TranslationEntry{"local temporary file size:", "本地临时文件大小："},
    TranslationEntry{"saved download progress:", "已保存下载进度："},
    TranslationEntry{"total file size:", "文件总大小："},
    TranslationEntry{"quota state:", "配额状态："},
    TranslationEntry{"configured drive:", "配置的驱动器："},
    TranslationEntry{"Graph delete operations:", "Graph 删除操作数："},
    TranslationEntry{"affected tracked items:", "受影响跟踪项数："},
    TranslationEntry{"configured limit:", "配置上限："},
    TranslationEntry{"override:", "强制覆盖："},
};

constexpr std::array kExactMessages{
    TranslationEntry{"Confirmation did not match.", "确认输入不匹配。"},
    TranslationEntry{"No saved blocked items.", "没有已保存的受阻项目。"},
    TranslationEntry{
        "No saved blocked items match the filters.",
        "没有符合过滤条件的受阻项目。",
    },
    TranslationEntry{
        "No partial downloads are recorded.", "没有已记录的分片下载。"
    },
    TranslationEntry{
        "No downloaded files matched the requested filters.",
        "没有符合过滤条件的已下载文件。",
    },
    TranslationEntry{
        "No shared resources or OneDrive shortcuts were found.",
        "未找到共享资源或 OneDrive 快捷方式。",
    },
    TranslationEntry{
        "No accessible SharePoint sites matched the query.",
        "未找到匹配的可用 SharePoint 站点。",
    },
    TranslationEntry{"Saved authentication removed.", "已移除保存的认证凭据。"},
    TranslationEntry{
        "No saved authentication was present.", "未发现已保存的认证凭据。"
    },
    TranslationEntry{
        "Waiting for authorization...", "正在等待登录授权..."
    },
    TranslationEntry{
        "Waiting for browser authorization...", "正在等待浏览器登录授权..."
    },
    TranslationEntry{
        "Browser authorization is unavailable; falling back to device code flow.",
        "浏览器登录授权不可用，正在回退至设备码认证流程。",
    },
    TranslationEntry{
        "WARNING: Authentication requests broad organizational file or SharePoint access. Keep Files.ReadWrite.All, Sites.Read.All, and Sites.ReadWrite.All only when the configured Drive or discovery workflow requires them.",
        "警告：当前认证请求了组织范围的宽泛文件或 SharePoint 权限。仅当配置的驱动器或 SharePoint 发现流程确实需要时，才应保留 Files.ReadWrite.All、Sites.Read.All 和 Sites.ReadWrite.All。",
    },
    TranslationEntry{
        "No synchronization state databases were found.",
        "未找到同步状态数据库。",
    },
    TranslationEntry{
        "All synchronization state databases passed full integrity and schema checks.",
        "所有同步状态数据库均已通过完整性与模式检查。",
    },
    TranslationEntry{
        "One or more synchronization state databases failed integrity or schema checks.",
        "一个或多个同步状态数据库未通过完整性或模式检查。",
    },
    TranslationEntry{
        "Database upgrade required; run 'state migrate --dry-run'.",
        "需要升级状态数据库；请先运行 'state migrate --dry-run' 查看。",
    },
    TranslationEntry{
        "Migration refused: one or more databases are unhealthy or have unsupported versions. No databases were upgraded.",
        "拒绝迁移：一个或多个数据库不健康或版本不受支持。未升级任何数据库。",
    },
    TranslationEntry{
        "Dry run: listed databases require an upgrade; no databases or backups were changed.",
        "仅预览：列出的数据库需要升级；未修改任何数据库或备份。",
    },
    TranslationEntry{
        "No databases require an upgrade.", "没有需要升级的数据库。"
    },
    TranslationEntry{
        "Database migration requires --yes with JSON output.",
        "使用 JSON 输出时，数据库迁移必须指定 --yes。",
    },
    TranslationEntry{
        "Database migration cancelled.", "数据库迁移已取消。"
    },
    TranslationEntry{
        "Database upgrades completed; no local or remote files were synchronized.",
        "数据库升级已完成；未执行任何本地或远端文件同步。",
    },
    TranslationEntry{
        "Cleanup requires --yes with JSON output.",
        "使用 JSON 输出时，清理操作必须指定 --yes。",
    },
    TranslationEntry{"Partial cleanup cancelled.", "分片下载清理已取消。"},
    TranslationEntry{
        "Local files will not be deleted, but the next sync may report local modification conflicts.",
        "本地文件不会被删除，但下次同步可能会报告本地修改冲突。",
    },
    TranslationEntry{
        "Local files were not deleted. The next sync may report local modification conflicts.",
        "本地文件未被删除。下次同步可能会报告本地修改冲突。",
    },
    TranslationEntry{
        "Interactive confirmation is unavailable with JSON output; use --yes to confirm explicitly.",
        "JSON 输出模式下无法进行交互式确认；请使用 --yes 显式确认。",
    },
    TranslationEntry{
        "Full state clear cancelled.", "清除全部同步状态操作已取消。"
    },
    TranslationEntry{
        "Item snapshots, pending downloads, partial downloads, pending uploads, pending moves, upload suppressions, and blocked items were preserved.",
        "已保留项目快照、待下载项、分片下载、待上传项、待移动项、上传抑制记录及受阻项目。",
    },
    TranslationEntry{
        "Saved pending remote delta metadata was discarded. The next sync will perform a full Microsoft Graph delta query.",
        "已丢弃保存的待处理远端增量元数据。下次同步将执行完整的 Microsoft Graph 增量查询。",
    },
    TranslationEntry{
        "Fetching Microsoft Graph changes...", "正在获取 Microsoft Graph 远端更改..."
    },
    TranslationEntry{
        "The saved Microsoft Graph cursor is no longer valid; fetching the full remote state...",
        "已保存的 Microsoft Graph 增量游标已失效，正在获取完整远端状态...",
    },
    TranslationEntry{
        "Saved remote changes no longer match the account, filters, hash policy, unsupported formats or applied cursor; fetching remote changes again...",
        "已保存的远端更改与当前账户、过滤规则、哈希策略、不支持格式或已应用游标不匹配，正在重新获取远端更改...",
    },
    TranslationEntry{
        "Preview metadata no longer matches the account, filters or synchronization state; fetching remote changes again...",
        "预览元数据与当前账户、过滤规则或同步状态不匹配，正在重新获取远端更改...",
    },
    TranslationEntry{
        "Reusing preview metadata; checking only subsequent remote changes and rebuilding the local plan...",
        "正在复用预览元数据，仅检查后续远端更改并重建本地同步计划...",
    },
    TranslationEntry{
        "Restoring saved remote changes; checking only subsequent changes and rebuilding the local plan...",
        "正在恢复已保存的远端更改，仅检查后续变更并重建本地同步计划...",
    },
    TranslationEntry{
        "Synchronization filters or unsupported-format rules changed; fetching the full remote state...",
        "同步过滤规则或不支持的格式规则已变更，正在获取完整远端状态...",
    },
    TranslationEntry{
        "Remote directory changes or unidentified deletions may affect saved paths; fetching the full remote state...",
        "远端目录变更或未识别的删除可能影响已保存路径，正在获取完整远端状态...",
    },
    TranslationEntry{"Sync complete.", "同步完成。"},
    TranslationEntry{
        "Synchronization completed.", "同步已完成。"
    },
    TranslationEntry{
        "Sync finished with items needing attention.",
        "同步已完成，但有部分项目需要关注。",
    },
};

} // namespace

bool is_chinese(Language language) noexcept {
    return resolve_language(language) == Language::simplified_chinese;
}

std::size_t display_width(std::string_view text) noexcept {
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80U) {
            ++width;
            ++index;
        } else if ((byte & 0xE0U) == 0xC0U) {
            ++width;
            index += 2;
        } else if ((byte & 0xF0U) == 0xE0U) {
            width += 2;
            index += 3;
        } else if ((byte & 0xF8U) == 0xF0U) {
            width += 2;
            index += 4;
        } else {
            ++width;
            ++index;
        }
    }
    return width;
}

std::string_view
translate_text(std::string_view english, Language language) noexcept {
    if (!is_chinese(language)) {
        return english;
    }
    if (const auto translated = lookup_entry(kTextTranslations, english)) {
        return *translated;
    }
    return english;
}

std::string translate_section_title(
    std::string_view event, std::string_view title, Language language
) {
    if (!is_chinese(language)) {
        return std::string{title};
    }
    if (const auto translated = lookup_entry(kSectionTitlesByEvent, event)) {
        return std::string{*translated};
    }
    return std::string{title};
}

std::string translate_field_label(
    std::string_view key, std::string_view label, Language language
) {
    if (!is_chinese(language)) {
        return std::string{label};
    }
    if (const auto translated = lookup_entry(kFieldLabelsByText, label)) {
        return std::string{*translated};
    }
    if (const auto translated = lookup_entry(kFieldLabelsByKey, key)) {
        return std::string{*translated};
    }
    return std::string{label};
}

std::string translate_field_value(
    std::string_view key, std::string_view value, Language language
) {
    if (!is_chinese(language)) {
        return std::string{value};
    }
    if (key == "reason_message") {
        return translate_blocked_reason({}, value, language);
    }
    if (key == "language") {
        if (value == "auto") {
            return "自动";
        }
        if (value == "en") {
            return "英文";
        }
        if (value == "zh_CN") {
            return "简体中文";
        }
    }
    return std::string{value};
}

std::string
translate_message(const MessageEvent& event, Language language) {
    if (!is_chinese(language)) {
        return event.text;
    }
    if (const auto exact = lookup_entry(kExactMessages, event.text)) {
        return std::string{*exact};
    }
    const std::string_view message{event.text};
    if (const auto token_path = extract_single(
            message,
            "Authentication succeeded. Refresh token saved to ",
            ""
        )) {
        return std::format("身份认证成功。刷新令牌已保存至 {}", *token_path);
    }
    if (const auto url = extract_single(
            message,
            "Opened your default browser for Microsoft sign-in. If it does not open, visit ",
            ""
        )) {
        return std::format(
            "已打开默认浏览器进行 Microsoft 登录。如果浏览器未自动打开，请访问 {}",
            *url
        );
    }
    if (const auto url = extract_single(
            message, "Open ", " in your browser to sign in."
        )) {
        return std::format("请在浏览器中打开 {} 完成登录。", *url);
    }
    if (const auto parts = extract_between(
            message, "Open ", " and enter code ", ""
        )) {
        return std::format(
            "请打开 {} 并输入验证码 {}", parts->first, parts->second
        );
    }
    if (const auto ack = extract_single(
            message,
            "Monitor request ",
            ". This acknowledges the request, not synchronization or shutdown completion."
        )) {
        return std::format(
            "监控控制请求已响应（{}）。此响应仅确认已接收请求，不代表同步或退出已完成。",
            *ack
        );
    }
    if (const auto drive = extract_single(
            message,
            "WARNING: This will remove all saved item snapshots, the Delta cursor, and pending-download recovery records for drive ",
            "."
        )) {
        return std::format(
            "警告：这将移除驱动器 {} 的所有已保存项目快照、增量游标及待下载恢复记录。",
            *drive
        );
    }
    if (const auto parts = extract_between(
            message,
            "Reset synchronization cursor for drive ",
            ": saved cursor ",
            "."
        )) {
        const auto cursor_state =
            parts->second == "removed" ? "已移除" : "不存在";
        return std::format(
            "已重置驱动器 {} 的同步游标：已保存游标{}。",
            parts->first,
            cursor_state
        );
    }
    if (const auto parts = extract_between(
            message, "Cannot upgrade '", "': ", ""
        )) {
        return std::format(
            "无法升级状态数据库 '{}'：{}", parts->first, parts->second
        );
    }
    if (const auto parts = extract_between(
            message, "Could not finish cleanup for '", "': ", ""
        )) {
        return std::format(
            "无法完成对 '{}' 的清理：{}", parts->first, parts->second
        );
    }
    if (const auto parts = extract_between(
            message, "Kept '", "': ", ""
        )) {
        return std::format("已保留 '{}'：{}", parts->first, parts->second);
    }
    if (const auto parts = extract_between(
            message, "Drive '", "': ", ""
        )) {
        return std::format("驱动器 '{}'：{}", parts->first, parts->second);
    }
    if (const auto path = extract_single(
            message,
            "Monitoring local and Microsoft Graph changes for:\n  ",
            ""
        )) {
        return std::format("正在监控本地与 Microsoft Graph 远端更改：\n  {}", *path);
    }
    if (const auto parts = extract_between(
            message,
            "Local changes settle for ",
            " milliseconds; remote WebSocket notifications trigger Delta synchronization, with Graph polling every ",
            " seconds as fallback."
        )) {
        return std::format(
            "本地更改防抖等待 {} 毫秒；远端 WebSocket 实时通知已启用，并以每 {} 秒轮询 Graph 作为回退。",
            parts->first,
            parts->second
        );
    }
    if (const auto parts = extract_between(
            message,
            "Local changes settle for ",
            " milliseconds; remote WebSocket notifications are disabled, and Graph is polled every ",
            " seconds."
        )) {
        return std::format(
            "本地更改防抖等待 {} 毫秒；远端 WebSocket 实时通知已禁用，每 {} 秒轮询一次 Graph。",
            parts->first,
            parts->second
        );
    }
    if (const auto parts = extract_between(
            message,
            "Remote deletion plan affects ",
            " tracked items, exceeding the configured limit of ",
            ". Proceeding because --force-large-delete was provided."
        )) {
        return std::format(
            "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。由于指定了 --force-large-delete，将继续执行。",
            parts->first,
            parts->second
        );
    }
    if (const auto parts = extract_between(
            message,
            "Remote deletion plan affects ",
            " tracked items, exceeding the configured limit of ",
            ". A normal sync would be blocked."
        )) {
        return std::format(
            "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。常规同步将被阻止。",
            parts->first,
            parts->second
        );
    }
    if (const auto parts = extract_between(
            message,
            "Remote deletion plan affects ",
            " tracked items, exceeding the configured limit of ",
            ". Review the local filesystem and rerun sync with --force-large-delete only if the deletions are intentional."
        )) {
        return std::format(
            "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。请检查本地文件系统，仅当确认需要删除时使用 --force-large-delete 重新运行。",
            parts->first,
            parts->second
        );
    }
    return event.text;
}

std::optional<std::string>
translate_tui_message(const MessageEvent& event, Language language) {
    if (event.event == "log" &&
        (event.kind == MessageKind::trace ||
         event.kind == MessageKind::debug ||
         event.kind == MessageKind::information)) {
        return std::nullopt;
    }
    if (!is_chinese(language)) {
        return std::nullopt;
    }
    if (event.event == "delta_query_started") {
        return "正在检查云端更改...";
    }
    if (event.event == "delta_cursor_invalid") {
        return "正在刷新云端变更历史...";
    }
    if (event.event == "sync_completed") {
        return "同步完成。";
    }
    if (event.event == "sync_completed_with_issues") {
        return "同步已完成，但有项目需要关注。";
    }
    if (event.event == "initial_delta_scheduled") {
        return "下次同步将执行完整的云端检查。";
    }
    return translate_message(event, language);
}

std::string translate_blocked_reason(
    std::string_view, std::string_view reason_message, Language language
) {
    if (!is_chinese(language)) {
        return std::string{reason_message};
    }
    if (const auto parts = extract_between(
            reason_message,
            "Remote deletion plan affects ",
            " tracked items, exceeding the configured limit of ",
            "."
        )) {
        return std::format(
            "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。",
            parts->first,
            parts->second
        );
    }
    return std::string{reason_message};
}

std::string format_delta_summary(
    const events::DeltaSummary& summary, Language language
) {
    const auto items_per_page =
        summary.pages == 0
            ? 0.0
            : static_cast<double>(summary.scanned_items) /
                  static_cast<double>(summary.pages);
    if (is_chinese(language)) {
        return std::format(
            "Microsoft Graph 增量同步完成：共 {} 页，扫描 {} 个项目，{} 个唯一更改（{} 个文件、{} 个文件夹、{} 个删除），平均 {:.1f} 项/页\n",
            summary.pages,
            summary.scanned_items,
            summary.unique_changes,
            summary.files,
            summary.directories,
            summary.deletions,
            items_per_page
        );
    }
    return std::format(
        "Microsoft Graph delta complete: {} page{}, {} items "
        "scanned, {} unique changes ({} files, {} folders, {} "
        "deletions), {:.1f} items/page\n",
        summary.pages,
        summary.pages == 1 ? "" : "s",
        summary.scanned_items,
        summary.unique_changes,
        summary.files,
        summary.directories,
        summary.deletions,
        items_per_page
    );
}

std::string format_blocked_item_line(
    const BlockedItemEvent& event, Language language
) {
    if (is_chinese(language)) {
        return std::format(
            "已受阻 '{}'：{} ({})\n",
            event.path,
            translate_blocked_reason(
                event.reason_code, event.reason_message, language
            ),
            event.reason_code
        );
    }
    return std::format(
        "Blocked '{}': {} ({})\n",
        event.path,
        event.reason_message,
        event.reason_code
    );
}

std::string translate_confirmation_prompt(
    const ConfirmationRequest& request, Language language
) {
    if (!is_chinese(language)) {
        return request.prompt;
    }
    const std::string_view prompt{request.prompt};
    if (prompt ==
        "Back up and upgrade all listed outdated databases? Type 'migrate' to confirm: ") {
        return "是否备份并升级上面列出的所有旧版本数据库？请输入 'migrate' 确认：";
    }
    if (const auto ref = extract_single(
            prompt,
            "Type the configured drive reference '",
            "' to confirm: "
        )) {
        return std::format("请输入配置的驱动器引用 '{}' 以确认：", *ref);
    }
    if (const auto ref = extract_single(
            prompt,
            "Type the configured drive reference '",
            "' to confirm cleanup: "
        )) {
        return std::format("请输入配置的驱动器引用 '{}' 以确认清理：", *ref);
    }
    return request.prompt;
}

std::string_view translate_file_action(
    events::FileAction action, Language language
) noexcept {
    if (!is_chinese(language)) {
        return events::file_action_name(action);
    }
    switch (action) {
    case events::FileAction::downloaded:
        return "已下载";
    case events::FileAction::uploaded:
        return "已上传";
    case events::FileAction::moved_locally:
        return "本地已移动";
    case events::FileAction::moved_remotely:
        return "远端已移动";
    case events::FileAction::deleted_locally:
        return "本地已删除";
    case events::FileAction::deleted_remotely:
        return "远端已删除";
    }
    return events::file_action_name(action);
}

std::string_view
translate_tui_view_title(TuiView view, Language language) noexcept {
    if (!is_chinese(language)) {
        switch (view) {
        case TuiView::auth:
            return "AUTH";
        case TuiView::health:
            return "HEALTH";
        case TuiView::status:
            return "STATUS";
        case TuiView::blocked:
            return "BLOCKED";
        case TuiView::drives:
            return "DRIVES";
        case TuiView::shared:
            return "SHARED";
        case TuiView::sites:
            return "SITES";
        case TuiView::quota:
            return "QUOTA";
        case TuiView::storage:
            return "STORAGE";
        case TuiView::partials:
            return "PARTIALS";
        case TuiView::files:
            return "FILES";
        case TuiView::verify:
            return "VERIFY";
        case TuiView::config:
            return "CONFIG";
        case TuiView::sync:
            return "SYNC";
        case TuiView::download:
            return "DOWNLOAD";
        case TuiView::watch:
            return "WATCH";
        }
        return "SYNC";
    }
    switch (view) {
    case TuiView::auth:
        return "认证";
    case TuiView::health:
        return "健康检查";
    case TuiView::status:
        return "同步状态";
    case TuiView::blocked:
        return "受阻项目";
    case TuiView::drives:
        return "驱动器";
    case TuiView::shared:
        return "共享项目";
    case TuiView::sites:
        return "站点";
    case TuiView::quota:
        return "存储配额";
    case TuiView::storage:
        return "本地存储";
    case TuiView::partials:
        return "分片下载";
    case TuiView::files:
        return "文件检查";
    case TuiView::verify:
        return "文件校验";
    case TuiView::config:
        return "有效配置";
    case TuiView::sync:
        return "同步";
    case TuiView::download:
        return "单文件下载";
    case TuiView::watch:
        return "实时监控";
    }
    return "同步";
}

std::string_view
translate_sync_phase(events::SyncPhase phase, Language language) noexcept {
    if (!is_chinese(language)) {
        return events::sync_phase_name(phase);
    }
    switch (phase) {
    case events::SyncPhase::preparing:
        return "准备中";
    case events::SyncPhase::scanning:
        return "扫描云端更改";
    case events::SyncPhase::scanning_local:
        return "扫描本地更改";
    case events::SyncPhase::downloading:
        return "下载中";
    case events::SyncPhase::uploading:
        return "上传中";
    case events::SyncPhase::applying:
        return "应用更改中";
    case events::SyncPhase::waiting:
        return "等待更改";
    }
    return events::sync_phase_name(phase);
}

std::string_view
translate_sync_mode(sync::SyncMode mode, Language language) noexcept {
    if (!is_chinese(language)) {
        return sync::sync_mode_name(mode);
    }
    switch (mode) {
    case sync::SyncMode::bidirectional:
        return "双向同步";
    case sync::SyncMode::download_only:
        return "仅下载";
    case sync::SyncMode::upload_only:
        return "仅上传";
    }
    return sync::sync_mode_name(mode);
}

} // namespace onedrive::cli::detail
