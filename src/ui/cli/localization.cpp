#include "ui/cli/localization.hpp"

#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace onedrive::cli::detail {
namespace {

struct TranslationEntry {
    std::string_view source;
    std::string_view target;
};

struct SingleArgRule {
    std::string_view prefix;
    std::string_view suffix;
    std::string_view pattern;
};

struct TwoArgRule {
    std::string_view prefix;
    std::string_view middle;
    std::string_view suffix;
    std::string_view pattern;
};

template <typename Enum>
struct EnumTranslation {
    Enum value;
    std::string_view text;
};

struct LocaleTemplates {
    std::string_view file_folder_tag;
    std::string_view file_recovered_tag;
    std::string_view file_local_path_pattern;
    std::string_view file_history_status_pattern;
    std::string_view file_history_drives_prefix;
    std::string_view delta_summary_singular_pattern;
    std::string_view delta_summary_plural_pattern;
    std::string_view blocked_item_line_pattern;
    std::string_view text_download_active_label;
    std::string_view text_download_done_label;
    std::string_view text_upload_active_label;
    std::string_view text_upload_done_label;
    std::string_view text_transfer_base_pattern;
    std::string_view text_transfer_rate_pattern;
    std::string_view text_transfer_elapsed_pattern;
    std::string_view text_transfer_eta_pattern;
    std::string_view text_sync_phase_pattern;
    std::string_view text_sync_context_pattern;
    std::string_view text_sync_preview_suffix;
    std::string_view tui_blocked_item_pattern;
    std::string_view tui_cloud_checked_pattern;
    std::string_view tui_cloud_changes_suffix_pattern;
    std::string_view tui_transfer_unknown_compact_pattern;
    std::string_view tui_transfer_heading_pattern;
    std::string_view tui_transfer_calculating_label;
    std::string_view tui_transfer_elapsed_suffix_pattern;
    std::string_view tui_transfer_eta_suffix_pattern;
    std::string_view tui_context_pattern;
    std::string_view tui_context_monitoring_label;
    std::string_view tui_context_one_time_label;
    std::string_view tui_context_preview_suffix;
    std::string_view tui_phase_pattern;
    std::string_view tui_phase_monitoring_label;
    std::string_view tui_phase_sync_label;
    std::string_view tui_summary_pattern;
    std::string_view tui_summary_incomplete_suffix;
    std::string_view tui_file_history_heading_pattern;
    std::string_view tui_paged_section_pattern;
    std::string_view tui_paged_drive_noun;
    std::string_view tui_paged_item_noun;
    std::string_view tui_partial_heading_pattern;
    std::string_view tui_needs_attention_pattern;
};

struct LocaleCatalog {
    Language language;
    CliFormatterLabels cli_labels;
    std::span<const TranslationEntry> text_entries;
    std::span<const TranslationEntry> section_titles_by_event;
    std::span<const TranslationEntry> field_labels_by_key;
    std::span<const TranslationEntry> field_labels_by_text;
    std::span<const TranslationEntry> language_value_names;
    std::span<const TranslationEntry> exact_messages;
    std::span<const SingleArgRule> single_arg_messages;
    std::span<const TwoArgRule> two_arg_messages;
    std::span<const TranslationEntry> cursor_states;
    std::optional<TwoArgRule> cursor_reset_rule;
    std::span<const TranslationEntry> tui_event_messages;
    std::span<const TranslationEntry> tui_message_replacements;
    std::span<const TranslationEntry> tui_term_replacements;
    std::span<const TwoArgRule> blocked_reason_rules;
    std::span<const TranslationEntry> exact_prompts;
    std::span<const SingleArgRule> single_arg_prompts;
    std::span<const EnumTranslation<events::FileAction>> file_actions;
    std::span<const EnumTranslation<TuiView>> tui_view_titles;
    std::span<const EnumTranslation<events::SyncPhase>> sync_phases;
    std::span<const EnumTranslation<sync::SyncMode>> sync_modes;
    LocaleTemplates templates;
};

[[nodiscard]] std::optional<std::string_view> lookup_entry(
    std::span<const TranslationEntry> table, std::string_view key
) noexcept {
    for (const auto& entry : table) {
        if (entry.source == key) {
            return entry.target;
        }
    }
    return std::nullopt;
}

template <typename Enum>
[[nodiscard]] std::optional<std::string_view> lookup_enum(
    std::span<const EnumTranslation<Enum>> table, Enum value
) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.text;
        }
    }
    return std::nullopt;
}

template <typename... Args>
[[nodiscard]] std::string format_runtime(
    std::string_view pattern, const Args&... args
) {
    return std::vformat(pattern, std::make_format_args(args...));
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

    // TUI & Console chrome & labels
    TranslationEntry{
        "Authorization received. Return to OneDrive C++; you may close this tab.",
        "已收到授权。请返回 OneDrive C++，可以关闭此标签页。",
    },
    TranslationEntry{
        "Invalid authorization callback.", "无效的授权回调。"
    },
    TranslationEntry{"Microsoft Graph delta: ", "Microsoft Graph 增量同步："},
    TranslationEntry{
        "UL: totals unknown; percentage unavailable\n",
        "上传：总量未知；无法计算百分比\n",
    },
    TranslationEntry{
        "Completed file operations (this process):\n"
        "Path  Action / details  Time (UTC)\n",
        "已完成的文件操作（当前进程）：\n"
        "路径  操作 / 详情  时间 (UTC)\n",
    },
    TranslationEntry{
        "\nCompleted file operations (this process):\n"
        "Path  Action / details  Time (UTC)\n",
        "\n已完成的文件操作（当前进程）：\n"
        "路径  操作 / 详情  时间 (UTC)\n",
    },
    TranslationEntry{"DOWNLOADS", "下载"},
    TranslationEntry{"UPLOADS", "上传"},
    TranslationEntry{"MONITORING", "实时监控"},
    TranslationEntry{"SYNC", "同步"},
    TranslationEntry{
        "Totals unknown; percentage unavailable.",
        "总量未知；无法计算百分比。",
    },
    TranslationEntry{"calculating...", "计算中..."},
    TranslationEntry{"  PATH", "  路径"},
    TranslationEntry{
        "STATUS  SAVED PROGRESS / TOTAL", "状态  已保存进度 / 总量"
    },
    TranslationEntry{
        "Last synchronization round incomplete.", "上一轮同步未完成。"
    },
    TranslationEntry{"ACTIVITY", "动态"},
    TranslationEntry{" q / Esc  EXIT ", " q / Esc  退出 "},
    TranslationEntry{
        " Up/Down or k/j: select file ", " 上/下方向键 或 k/j：选择文件 "
    },
    TranslationEntry{
        " Left/Right or n/p: previous/next Drive ",
        " 左/右方向键 或 n/p：上/下一个驱动器 ",
    },
    TranslationEntry{
        " Left/Right or n/p: previous/next item ",
        " 左/右方向键 或 n/p：上/下一个项目 ",
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

constexpr std::array kChineseLanguageValueNames{
    TranslationEntry{"auto", "自动"},
    TranslationEntry{"en", "英文"},
    TranslationEntry{"zh_CN", "简体中文"},
};

constexpr std::array kChineseSingleArgMessages{
    SingleArgRule{
        "Authentication succeeded. Refresh token saved to ",
        "",
        "身份认证成功。刷新令牌已保存至 {}",
    },
    SingleArgRule{
        "Opened your default browser for Microsoft sign-in. If it does not open, visit ",
        "",
        "已打开默认浏览器进行 Microsoft 登录。如果浏览器未自动打开，请访问 {}",
    },
    SingleArgRule{
        "Open ",
        " in your browser to sign in.",
        "请在浏览器中打开 {} 完成登录。",
    },
    SingleArgRule{
        "Monitor request ",
        ". This acknowledges the request, not synchronization or shutdown completion.",
        "监控控制请求已响应（{}）。此响应仅确认已接收请求，不代表同步或退出已完成。",
    },
    SingleArgRule{
        "WARNING: This will remove all saved item snapshots, the Delta cursor, and pending-download recovery records for drive ",
        ".",
        "警告：这将移除驱动器 {} 的所有已保存项目快照、增量游标及待下载恢复记录。",
    },
    SingleArgRule{
        "Monitoring local and Microsoft Graph changes for:\n  ",
        "",
        "正在监控本地与 Microsoft Graph 远端更改：\n  {}",
    },
};

constexpr std::array kChineseTwoArgMessages{
    TwoArgRule{
        "Open ",
        " and enter code ",
        "",
        "请打开 {} 并输入验证码 {}",
    },
    TwoArgRule{
        "Cannot upgrade '",
        "': ",
        "",
        "无法升级状态数据库 '{}'：{}",
    },
    TwoArgRule{
        "Could not finish cleanup for '",
        "': ",
        "",
        "无法完成对 '{}' 的清理：{}",
    },
    TwoArgRule{
        "Kept '",
        "': ",
        "",
        "已保留 '{}'：{}",
    },
    TwoArgRule{
        "Drive '",
        "': ",
        "",
        "驱动器 '{}'：{}",
    },
    TwoArgRule{
        "Local changes settle for ",
        " milliseconds; remote WebSocket notifications trigger Delta synchronization, with Graph polling every ",
        " seconds as fallback.",
        "本地更改防抖等待 {} 毫秒；远端 WebSocket 实时通知已启用，并以每 {} 秒轮询 Graph 作为回退。",
    },
    TwoArgRule{
        "Local changes settle for ",
        " milliseconds; remote WebSocket notifications are disabled, and Graph is polled every ",
        " seconds.",
        "本地更改防抖等待 {} 毫秒；远端 WebSocket 实时通知已禁用，每 {} 秒轮询一次 Graph。",
    },
    TwoArgRule{
        "Remote deletion plan affects ",
        " tracked items, exceeding the configured limit of ",
        ". Proceeding because --force-large-delete was provided.",
        "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。由于指定了 --force-large-delete，将继续执行。",
    },
    TwoArgRule{
        "Remote deletion plan affects ",
        " tracked items, exceeding the configured limit of ",
        ". A normal sync would be blocked.",
        "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。常规同步将被阻止。",
    },
    TwoArgRule{
        "Remote deletion plan affects ",
        " tracked items, exceeding the configured limit of ",
        ". Review the local filesystem and rerun sync with --force-large-delete only if the deletions are intentional.",
        "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。请检查本地文件系统，仅当确认需要删除时使用 --force-large-delete 重新运行。",
    },
};

constexpr std::array kChineseCursorStates{
    TranslationEntry{"removed", "已移除"},
    TranslationEntry{"not present", "不存在"},
};

constexpr std::array kEnglishTuiEventMessages{
    TranslationEntry{
        "delta_query_started", "Checking the cloud for changes..."
    },
    TranslationEntry{"delta_cursor_invalid", "Refreshing cloud history..."},
    TranslationEntry{"sync_completed", "Sync complete."},
    TranslationEntry{
        "sync_completed_with_issues",
        "Sync finished with items needing attention.",
    },
    TranslationEntry{
        "initial_delta_scheduled",
        "The next sync will perform a full cloud check.",
    },
};

constexpr std::array kChineseTuiEventMessages{
    TranslationEntry{"delta_query_started", "正在检查云端变更..."},
    TranslationEntry{"delta_cursor_invalid", "正在刷新云端历史记录..."},
    TranslationEntry{"sync_completed", "同步完成。"},
    TranslationEntry{
        "sync_completed_with_issues",
        "同步已完成，但有项目需要关注。",
    },
    TranslationEntry{
        "initial_delta_scheduled",
        "下次同步将执行完整云端检查。",
    },
};

constexpr std::array kEnglishTuiMessageReplacements{
    TranslationEntry{"Local changes settle for ", "Local changes wait "},
    TranslationEntry{
        " milliseconds; remote WebSocket notifications trigger Delta "
        "synchronization, with Graph polling every ",
        " milliseconds to settle; live cloud updates are enabled, with a "
        "safety check every ",
    },
    TranslationEntry{
        " milliseconds; remote WebSocket notifications are disabled, and "
        "Graph is polled every ",
        " milliseconds to settle; live cloud updates are disabled, so "
        "changes are checked every ",
    },
    TranslationEntry{" seconds as fallback.", " seconds."},
    TranslationEntry{
        "Monitoring local and Microsoft Graph changes for:",
        "Watching for local and cloud changes in:",
    },
    TranslationEntry{"Remote delta contains", "Cloud storage has"},
    TranslationEntry{" upserts", " new or updated"},
    TranslationEntry{" removals", " removed"},
    TranslationEntry{" moves", " moved"},
    TranslationEntry{" blocked", " need attention"},
    TranslationEntry{"the Delta cursor", "cloud change history"},
};

constexpr std::array kEnglishTuiTermReplacements{
    TranslationEntry{"Microsoft Graph", "cloud service"},
    TranslationEntry{"Graph", "cloud service"},
    TranslationEntry{"WebSocket", "live cloud updates"},
    TranslationEntry{"Delta", "cloud check"},
    TranslationEntry{"delta", "cloud check"},
};

constexpr std::array kChineseBlockedReasonRules{
    TwoArgRule{
        "Remote deletion plan affects ",
        " tracked items, exceeding the configured limit of ",
        ".",
        "远端删除计划涉及 {} 个受跟踪项目，超过配置上限 {}。",
    },
};

constexpr std::array kChineseExactPrompts{
    TranslationEntry{
        "Back up and upgrade all listed outdated databases? Type 'migrate' to confirm: ",
        "是否备份并升级上面列出的所有旧版本数据库？请输入 'migrate' 确认：",
    },
};

constexpr std::array kChineseSingleArgPrompts{
    SingleArgRule{
        "Type the configured drive reference '",
        "' to confirm: ",
        "请输入配置的驱动器引用 '{}' 以确认：",
    },
    SingleArgRule{
        "Type the configured drive reference '",
        "' to confirm cleanup: ",
        "请输入配置的驱动器引用 '{}' 以确认清理：",
    },
};

constexpr std::array kChineseFileActions{
    EnumTranslation<events::FileAction>{events::FileAction::downloaded, "已下载"},
    EnumTranslation<events::FileAction>{events::FileAction::uploaded, "已上传"},
    EnumTranslation<events::FileAction>{
        events::FileAction::moved_locally, "本地已移动"
    },
    EnumTranslation<events::FileAction>{
        events::FileAction::moved_remotely, "远端已移动"
    },
    EnumTranslation<events::FileAction>{
        events::FileAction::deleted_locally, "本地已删除"
    },
    EnumTranslation<events::FileAction>{
        events::FileAction::deleted_remotely, "远端已删除"
    },
};

constexpr std::array kEnglishTuiViewTitles{
    EnumTranslation<TuiView>{TuiView::auth, "AUTH"},
    EnumTranslation<TuiView>{TuiView::health, "HEALTH"},
    EnumTranslation<TuiView>{TuiView::status, "STATUS"},
    EnumTranslation<TuiView>{TuiView::blocked, "BLOCKED"},
    EnumTranslation<TuiView>{TuiView::drives, "DRIVES"},
    EnumTranslation<TuiView>{TuiView::shared, "SHARED"},
    EnumTranslation<TuiView>{TuiView::sites, "SITES"},
    EnumTranslation<TuiView>{TuiView::quota, "QUOTA"},
    EnumTranslation<TuiView>{TuiView::storage, "STORAGE"},
    EnumTranslation<TuiView>{TuiView::partials, "PARTIALS"},
    EnumTranslation<TuiView>{TuiView::files, "FILES"},
    EnumTranslation<TuiView>{TuiView::verify, "VERIFY"},
    EnumTranslation<TuiView>{TuiView::config, "CONFIG"},
    EnumTranslation<TuiView>{TuiView::sync, "SYNC"},
    EnumTranslation<TuiView>{TuiView::download, "DOWNLOAD"},
    EnumTranslation<TuiView>{TuiView::watch, "WATCH"},
};

constexpr std::array kChineseTuiViewTitles{
    EnumTranslation<TuiView>{TuiView::auth, "认证"},
    EnumTranslation<TuiView>{TuiView::health, "健康检查"},
    EnumTranslation<TuiView>{TuiView::status, "状态"},
    EnumTranslation<TuiView>{TuiView::blocked, "受阻项目"},
    EnumTranslation<TuiView>{TuiView::drives, "驱动器"},
    EnumTranslation<TuiView>{TuiView::shared, "共享内容"},
    EnumTranslation<TuiView>{TuiView::sites, "站点"},
    EnumTranslation<TuiView>{TuiView::quota, "配额"},
    EnumTranslation<TuiView>{TuiView::storage, "存储占用"},
    EnumTranslation<TuiView>{TuiView::partials, "未完成下载"},
    EnumTranslation<TuiView>{TuiView::files, "已下载文件"},
    EnumTranslation<TuiView>{TuiView::verify, "文件校验"},
    EnumTranslation<TuiView>{TuiView::config, "配置"},
    EnumTranslation<TuiView>{TuiView::sync, "同步"},
    EnumTranslation<TuiView>{TuiView::download, "下载"},
    EnumTranslation<TuiView>{TuiView::watch, "监控"},
};

constexpr std::array kChineseSyncPhases{
    EnumTranslation<events::SyncPhase>{events::SyncPhase::preparing, "准备中"},
    EnumTranslation<events::SyncPhase>{events::SyncPhase::scanning, "扫描云端更改"},
    EnumTranslation<events::SyncPhase>{
        events::SyncPhase::scanning_local, "扫描本地更改"
    },
    EnumTranslation<events::SyncPhase>{events::SyncPhase::downloading, "下载中"},
    EnumTranslation<events::SyncPhase>{events::SyncPhase::uploading, "上传中"},
    EnumTranslation<events::SyncPhase>{events::SyncPhase::applying, "应用更改中"},
    EnumTranslation<events::SyncPhase>{events::SyncPhase::waiting, "等待更改"},
};

constexpr std::array kChineseSyncModes{
    EnumTranslation<sync::SyncMode>{sync::SyncMode::bidirectional, "双向同步"},
    EnumTranslation<sync::SyncMode>{sync::SyncMode::download_only, "仅下载"},
    EnumTranslation<sync::SyncMode>{sync::SyncMode::upload_only, "仅上传"},
};

constexpr LocaleCatalog kEnglishCatalog{
    .language = Language::english,
    .cli_labels =
        CliFormatterLabels{
            .usage = "Usage",
            .options_upper = "OPTIONS",
            .options = "Options",
            .subcommands_upper = "SUBCOMMANDS",
            .subcommands = "Subcommands",
            .positionals = "Positionals",
            .required = "REQUIRED",
        },
    .text_entries = {},
    .section_titles_by_event = {},
    .field_labels_by_key = {},
    .field_labels_by_text = {},
    .language_value_names = {},
    .exact_messages = {},
    .single_arg_messages = {},
    .two_arg_messages = {},
    .cursor_states = {},
    .cursor_reset_rule = std::nullopt,
    .tui_event_messages = kEnglishTuiEventMessages,
    .tui_message_replacements = kEnglishTuiMessageReplacements,
    .tui_term_replacements = kEnglishTuiTermReplacements,
    .blocked_reason_rules = {},
    .exact_prompts = {},
    .single_arg_prompts = {},
    .file_actions = {},
    .tui_view_titles = kEnglishTuiViewTitles,
    .sync_phases = {},
    .sync_modes = {},
    .templates =
        LocaleTemplates{
            .file_folder_tag = " (folder)",
            .file_recovered_tag = " (recovered)",
            .file_local_path_pattern = " [local: {}]",
            .file_history_status_pattern =
                "File history: {} retained (limit {}); {} older operations omitted.",
            .file_history_drives_prefix = "\nDrives (retained history): ",
            .delta_summary_singular_pattern =
                "Microsoft Graph delta complete: {} page, {} items scanned, {} unique changes ({} files, {} folders, {} deletions), {:.1f} items/page\n",
            .delta_summary_plural_pattern =
                "Microsoft Graph delta complete: {} pages, {} items scanned, {} unique changes ({} files, {} folders, {} deletions), {:.1f} items/page\n",
            .blocked_item_line_pattern = "Blocked '{}': {} ({})\n",
            .text_download_active_label = "DL",
            .text_download_done_label = "Done",
            .text_upload_active_label = "UL",
            .text_upload_done_label = "Upload done",
            .text_transfer_base_pattern = "{}: {}/{} files, {}% ({}/{})",
            .text_transfer_rate_pattern = ", {}/s",
            .text_transfer_elapsed_pattern = ", elapsed {}",
            .text_transfer_eta_pattern = ", ETA {}",
            .text_sync_phase_pattern = "Phase: {}\n",
            .text_sync_context_pattern = "Synchronization: {}{}\n",
            .text_sync_preview_suffix = " (preview)",
            .tui_blocked_item_pattern = "{}: {}",
            .tui_cloud_checked_pattern = "CLOUD CHECK  {} items checked",
            .tui_cloud_changes_suffix_pattern = "  //  {} changes found",
            .tui_transfer_unknown_compact_pattern = "{} // totals unknown",
            .tui_transfer_heading_pattern = "{}  {}/{} files",
            .tui_transfer_calculating_label = "calculating...",
            .tui_transfer_elapsed_suffix_pattern = "  elapsed {}",
            .tui_transfer_eta_suffix_pattern = "  ETA {}",
            .tui_context_pattern = "Direction: {} // {}{}",
            .tui_context_monitoring_label = "automatic monitoring",
            .tui_context_one_time_label = "one-time",
            .tui_context_preview_suffix = " // preview",
            .tui_phase_pattern = "{}: {}",
            .tui_phase_monitoring_label = "MONITORING",
            .tui_phase_sync_label = "SYNC",
            .tui_summary_pattern =
                "Last round: {} downloaded, {} uploaded, {} moved, {} deleted, {} blocked{}",
            .tui_summary_incomplete_suffix = " // incomplete",
            .tui_file_history_heading_pattern =
                "FILES // {} retained // {} older omitted",
            .tui_paged_section_pattern = "{}  [{} {}/{}]",
            .tui_paged_drive_noun = "Drive",
            .tui_paged_item_noun = "Item",
            .tui_partial_heading_pattern = "Partial downloads [File {}/{}]",
            .tui_needs_attention_pattern = "NEEDS ATTENTION  {}  //  {}",
        },
};

constexpr LocaleCatalog kSimplifiedChineseCatalog{
    .language = Language::simplified_chinese,
    .cli_labels =
        CliFormatterLabels{
            .usage = "用法",
            .options_upper = "选项",
            .options = "选项",
            .subcommands_upper = "子命令",
            .subcommands = "子命令",
            .positionals = "位置参数",
            .required = "必填",
        },
    .text_entries = kTextTranslations,
    .section_titles_by_event = kSectionTitlesByEvent,
    .field_labels_by_key = kFieldLabelsByKey,
    .field_labels_by_text = kFieldLabelsByText,
    .language_value_names = kChineseLanguageValueNames,
    .exact_messages = kExactMessages,
    .single_arg_messages = kChineseSingleArgMessages,
    .two_arg_messages = kChineseTwoArgMessages,
    .cursor_states = kChineseCursorStates,
    .cursor_reset_rule =
        TwoArgRule{
            "Reset synchronization cursor for drive ",
            ": saved cursor ",
            ".",
            "已重置驱动器 {} 的同步游标：已保存游标{}。",
        },
    .tui_event_messages = kChineseTuiEventMessages,
    .tui_message_replacements = {},
    .tui_term_replacements = {},
    .blocked_reason_rules = kChineseBlockedReasonRules,
    .exact_prompts = kChineseExactPrompts,
    .single_arg_prompts = kChineseSingleArgPrompts,
    .file_actions = kChineseFileActions,
    .tui_view_titles = kChineseTuiViewTitles,
    .sync_phases = kChineseSyncPhases,
    .sync_modes = kChineseSyncModes,
    .templates =
        LocaleTemplates{
            .file_folder_tag = "（文件夹）",
            .file_recovered_tag = "（已恢复）",
            .file_local_path_pattern = " [本地：{}]",
            .file_history_status_pattern =
                "文件历史记录：保留 {} 条（上限 {}）；已省略 {} 条更早的操作。",
            .file_history_drives_prefix = "\n驱动器（保留的历史记录）：",
            .delta_summary_singular_pattern =
                "Microsoft Graph 增量同步完成：共 {} 页，扫描 {} 个项目，{} 个唯一更改（{} 个文件、{} 个文件夹、{} 个删除），平均 {:.1f} 项/页\n",
            .delta_summary_plural_pattern =
                "Microsoft Graph 增量同步完成：共 {} 页，扫描 {} 个项目，{} 个唯一更改（{} 个文件、{} 个文件夹、{} 个删除），平均 {:.1f} 项/页\n",
            .blocked_item_line_pattern = "已受阻 '{}'：{} ({})\n",
            .text_download_active_label = "下载",
            .text_download_done_label = "完成",
            .text_upload_active_label = "上传",
            .text_upload_done_label = "上传完成",
            .text_transfer_base_pattern = "{}：{}/{} 个文件，{}%（{}/{}）",
            .text_transfer_rate_pattern = "，{}/s",
            .text_transfer_elapsed_pattern = "，用时 {}",
            .text_transfer_eta_pattern = "，预计剩余 {}",
            .text_sync_phase_pattern = "阶段：{}\n",
            .text_sync_context_pattern = "同步：{}{}\n",
            .text_sync_preview_suffix = "（预览）",
            .tui_blocked_item_pattern = "{}：{}",
            .tui_cloud_checked_pattern = "云端检查  已检查 {} 项",
            .tui_cloud_changes_suffix_pattern = "  //  发现 {} 处变更",
            .tui_transfer_unknown_compact_pattern = "{} // 总量未知",
            .tui_transfer_heading_pattern = "{}  {}/{} 个文件",
            .tui_transfer_calculating_label = "计算中...",
            .tui_transfer_elapsed_suffix_pattern = "  用时 {}",
            .tui_transfer_eta_suffix_pattern = "  预计剩余 {}",
            .tui_context_pattern = "方向：{} // {}{}",
            .tui_context_monitoring_label = "自动监控",
            .tui_context_one_time_label = "单次执行",
            .tui_context_preview_suffix = " // 预览",
            .tui_phase_pattern = "{}：{}",
            .tui_phase_monitoring_label = "监控状态",
            .tui_phase_sync_label = "同步状态",
            .tui_summary_pattern =
                "上一轮：下载 {}，上传 {}，移动 {}，删除 {}，受阻 {}{}",
            .tui_summary_incomplete_suffix = " // 未完成",
            .tui_file_history_heading_pattern =
                "文件记录 // 保留 {} 条 // 省略 {} 条更早记录",
            .tui_paged_section_pattern = "{}  [{} {}/{}]",
            .tui_paged_drive_noun = "驱动器",
            .tui_paged_item_noun = "项目",
            .tui_partial_heading_pattern = "未完成下载 [文件 {}/{}]",
            .tui_needs_attention_pattern = "需要关注  {}  //  {}",
        },
};

constexpr std::array kLocaleCatalogs{
    kEnglishCatalog,
    kSimplifiedChineseCatalog,
};

[[nodiscard]] const LocaleCatalog& catalog_for(Language language) noexcept {
    const auto resolved = resolve_language(language);
    for (const auto& catalog : kLocaleCatalogs) {
        if (catalog.language == resolved) {
            return catalog;
        }
    }
    return kEnglishCatalog;
}

[[nodiscard]] std::string apply_replacements(
    std::string text, std::span<const TranslationEntry> replacements
) {
    for (const auto& entry : replacements) {
        std::size_t pos = 0;
        while ((pos = text.find(entry.source, pos)) != std::string::npos) {
            text.replace(pos, entry.source.size(), entry.target);
            pos += entry.target.size();
        }
    }
    return text;
}

} // namespace

std::size_t display_width(std::string_view text) noexcept {
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xE0U) == 0xC0U) {
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

CliFormatterLabels cli_formatter_labels(Language language) noexcept {
    return catalog_for(language).cli_labels;
}

std::string_view
translate_text(std::string_view english, Language language) noexcept {
    const auto& catalog = catalog_for(language);
    if (const auto translated = lookup_entry(catalog.text_entries, english)) {
        return *translated;
    }
    return english;
}

std::string translate_section_title(
    std::string_view event, std::string_view title, Language language
) {
    const auto& catalog = catalog_for(language);
    if (const auto translated =
            lookup_entry(catalog.section_titles_by_event, event)) {
        return std::string{*translated};
    }
    return std::string{title};
}

std::string translate_field_label(
    std::string_view key, std::string_view label, Language language
) {
    const auto& catalog = catalog_for(language);
    if (const auto translated =
            lookup_entry(catalog.field_labels_by_text, label)) {
        return std::string{*translated};
    }
    if (const auto translated =
            lookup_entry(catalog.field_labels_by_key, key)) {
        return std::string{*translated};
    }
    return std::string{label};
}

std::string translate_tui_section_title(
    std::string_view event, std::string_view title, Language language
) {
    const auto& catalog = catalog_for(language);
    return apply_replacements(
        translate_section_title(event, title, language),
        catalog.tui_term_replacements
    );
}

std::string translate_tui_field_label(
    std::string_view key, std::string_view label, Language language
) {
    const auto& catalog = catalog_for(language);
    return apply_replacements(
        translate_field_label(key, label, language),
        catalog.tui_term_replacements
    );
}

std::string translate_field_value(
    std::string_view key, std::string_view value, Language language
) {
    const auto& catalog = catalog_for(language);
    if (key == "reason_message") {
        return translate_blocked_reason({}, value, language);
    }
    if (key == "language") {
        if (const auto translated =
                lookup_entry(catalog.language_value_names, value)) {
            return std::string{*translated};
        }
    }
    return std::string{value};
}

std::string
translate_message(const MessageEvent& event, Language language) {
    const auto& catalog = catalog_for(language);
    if (const auto exact = lookup_entry(catalog.exact_messages, event.text)) {
        return std::string{*exact};
    }
    const std::string_view message{event.text};
    for (const auto& rule : catalog.single_arg_messages) {
        if (const auto arg = extract_single(message, rule.prefix, rule.suffix)) {
            return format_runtime(rule.pattern, *arg);
        }
    }
    if (catalog.cursor_reset_rule) {
        if (const auto parts = extract_between(
                message,
                catalog.cursor_reset_rule->prefix,
                catalog.cursor_reset_rule->middle,
                catalog.cursor_reset_rule->suffix
            )) {
            const auto state =
                lookup_entry(catalog.cursor_states, parts->second)
                    .value_or(parts->second);
            return format_runtime(
                catalog.cursor_reset_rule->pattern, parts->first, state
            );
        }
    }
    for (const auto& rule : catalog.two_arg_messages) {
        if (const auto parts = extract_between(
                message, rule.prefix, rule.middle, rule.suffix
            )) {
            return format_runtime(rule.pattern, parts->first, parts->second);
        }
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
    const auto& catalog = catalog_for(language);
    if (const auto by_event =
            lookup_entry(catalog.tui_event_messages, event.event)) {
        return std::string{*by_event};
    }
    auto translated = apply_replacements(
        translate_message(event, language), catalog.tui_message_replacements
    );
    return apply_replacements(
        std::move(translated), catalog.tui_term_replacements
    );
}

std::string translate_blocked_reason(
    std::string_view, std::string_view reason_message, Language language
) {
    const auto& catalog = catalog_for(language);
    for (const auto& rule : catalog.blocked_reason_rules) {
        if (const auto parts = extract_between(
                reason_message, rule.prefix, rule.middle, rule.suffix
            )) {
            return format_runtime(rule.pattern, parts->first, parts->second);
        }
    }
    return std::string{reason_message};
}

std::string format_tui_blocked_item(
    const BlockedItemEvent& event, Language language
) {
    const auto& catalog = catalog_for(language);
    const auto reason = apply_replacements(
        translate_blocked_reason(
            event.reason_code, event.reason_message, language
        ),
        catalog.tui_term_replacements
    );
    return format_runtime(
        catalog.templates.tui_blocked_item_pattern, event.path, reason
    );
}

std::string format_file_operation_line(
    std::string_view formatted_path,
    events::FileAction action,
    bool directory,
    bool recovered,
    std::string_view local_path,
    std::string_view completed_at,
    Language language
) {
    const auto& catalog = catalog_for(language);
    const auto local_suffix =
        local_path.empty()
            ? std::string{}
            : format_runtime(
                  catalog.templates.file_local_path_pattern, local_path
              );
    return format_runtime(
        "{}  {}{}{}{}  {}",
        formatted_path,
        translate_file_action(action, language),
        directory ? catalog.templates.file_folder_tag : std::string_view{},
        recovered ? catalog.templates.file_recovered_tag : std::string_view{},
        local_suffix,
        completed_at
    );
}

std::string format_file_history_summary(
    std::size_t retained,
    std::size_t limit,
    std::size_t omitted,
    std::string_view drives_csv,
    Language language
) {
    const auto& catalog = catalog_for(language);
    auto status = format_runtime(
        catalog.templates.file_history_status_pattern, retained, limit, omitted
    );
    if (!drives_csv.empty()) {
        status += catalog.templates.file_history_drives_prefix;
        status += drives_csv;
    }
    return status;
}

std::string format_delta_summary(
    const events::DeltaSummary& summary, Language language
) {
    const auto& catalog = catalog_for(language);
    const auto items_per_page =
        summary.pages == 0
            ? 0.0
            : static_cast<double>(summary.scanned_items) /
                  static_cast<double>(summary.pages);
    const auto pattern =
        summary.pages == 1
            ? catalog.templates.delta_summary_singular_pattern
            : catalog.templates.delta_summary_plural_pattern;
    return format_runtime(
        pattern,
        summary.pages,
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
    const auto& catalog = catalog_for(language);
    const auto reason = translate_blocked_reason(
        event.reason_code, event.reason_message, language
    );
    return format_runtime(
        catalog.templates.blocked_item_line_pattern,
        event.path,
        reason,
        event.reason_code
    );
}

std::string format_text_transfer_progress(
    bool upload,
    bool completed,
    std::size_t completed_files,
    std::size_t file_count,
    unsigned int percentage,
    std::string_view transferred_bytes,
    std::string_view total_bytes,
    std::optional<std::string_view> rate_bytes,
    std::optional<std::string_view> duration,
    Language language
) {
    const auto& catalog = catalog_for(language);
    const auto label =
        upload ? (completed ? catalog.templates.text_upload_done_label
                            : catalog.templates.text_upload_active_label)
               : (completed ? catalog.templates.text_download_done_label
                            : catalog.templates.text_download_active_label);
    auto line = format_runtime(
        catalog.templates.text_transfer_base_pattern,
        label,
        completed_files,
        file_count,
        percentage,
        transferred_bytes,
        total_bytes
    );
    if (rate_bytes) {
        line += format_runtime(
            catalog.templates.text_transfer_rate_pattern, *rate_bytes
        );
    }
    if (completed && duration) {
        line += format_runtime(
            catalog.templates.text_transfer_elapsed_pattern, *duration
        );
    } else if (!completed && duration) {
        line += format_runtime(
            catalog.templates.text_transfer_eta_pattern, *duration
        );
    }
    return line;
}

std::string format_text_sync_phase_line(
    events::SyncPhase phase, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.text_sync_phase_pattern,
        translate_sync_phase(phase, language)
    );
}

std::string format_text_sync_context_line(
    sync::SyncMode direction, bool preview, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.text_sync_context_pattern,
        translate_sync_mode(direction, language),
        preview ? catalog.templates.text_sync_preview_suffix
                : std::string_view{}
    );
}

std::string format_tui_cloud_status(
    std::size_t scanned_items,
    std::optional<std::size_t> changes_found,
    Language language
) {
    const auto& catalog = catalog_for(language);
    auto status = format_runtime(
        catalog.templates.tui_cloud_checked_pattern, scanned_items
    );
    if (changes_found) {
        status += format_runtime(
            catalog.templates.tui_cloud_changes_suffix_pattern, *changes_found
        );
    }
    return status;
}

std::string format_tui_transfer_unknown_compact(
    std::string_view title, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.tui_transfer_unknown_compact_pattern, title
    );
}

std::string format_tui_transfer_heading(
    std::string_view title,
    std::size_t completed_files,
    std::size_t file_count,
    Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.tui_transfer_heading_pattern,
        title,
        completed_files,
        file_count
    );
}

std::string format_tui_transfer_details(
    unsigned int percentage,
    std::string_view transferred_bytes,
    std::string_view total_bytes,
    std::string_view rate_bytes,
    bool completed,
    std::optional<std::string_view> duration,
    Language language
) {
    const auto& catalog = catalog_for(language);
    auto details = format_runtime(
        "{}%  {} / {}  {}/s",
        percentage,
        transferred_bytes,
        total_bytes,
        rate_bytes
    );
    if (completed) {
        const auto elapsed = duration.value_or("<1s");
        details += format_runtime(
            catalog.templates.tui_transfer_elapsed_suffix_pattern, elapsed
        );
    } else {
        const auto eta =
            duration.value_or(catalog.templates.tui_transfer_calculating_label);
        details += format_runtime(
            catalog.templates.tui_transfer_eta_suffix_pattern, eta
        );
    }
    return details;
}

std::string format_tui_sync_context(
    sync::SyncMode direction, bool monitoring, bool preview, Language language
) {
    const auto& catalog = catalog_for(language);
    const auto run_kind =
        monitoring ? catalog.templates.tui_context_monitoring_label
                   : catalog.templates.tui_context_one_time_label;
    const auto mode_text = translate_sync_mode(direction, language);
    const auto preview_suffix =
        preview ? catalog.templates.tui_context_preview_suffix
                : std::string_view{};
    return format_runtime(
        catalog.templates.tui_context_pattern,
        mode_text,
        run_kind,
        preview_suffix
    );
}

std::string format_tui_sync_phase(
    bool monitoring, events::SyncPhase phase, Language language
) {
    const auto& catalog = catalog_for(language);
    const auto prefix =
        monitoring ? catalog.templates.tui_phase_monitoring_label
                   : catalog.templates.tui_phase_sync_label;
    const auto phase_text = translate_sync_phase(phase, language);
    return format_runtime(
        catalog.templates.tui_phase_pattern, prefix, phase_text
    );
}

std::string format_tui_sync_summary(
    const events::SyncSummaryEvent& summary, Language language
) {
    const auto& catalog = catalog_for(language);
    const auto incomplete_suffix =
        summary.blocked != 0 ? catalog.templates.tui_summary_incomplete_suffix
                             : std::string_view{};
    return format_runtime(
        catalog.templates.tui_summary_pattern,
        summary.downloaded,
        summary.uploaded,
        summary.moved,
        summary.deleted,
        summary.blocked,
        incomplete_suffix
    );
}

std::string format_tui_file_history_heading(
    std::size_t retained, std::size_t omitted, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.tui_file_history_heading_pattern, retained, omitted
    );
}

std::string format_tui_paged_section_title(
    std::string_view title,
    TuiView view,
    std::size_t index,
    std::size_t total,
    Language language
) {
    const auto& catalog = catalog_for(language);
    const auto noun = view == TuiView::drives
                          ? catalog.templates.tui_paged_drive_noun
                          : catalog.templates.tui_paged_item_noun;
    return format_runtime(
        catalog.templates.tui_paged_section_pattern, title, noun, index, total
    );
}

std::string format_tui_partial_heading(
    std::size_t index, std::size_t total, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.tui_partial_heading_pattern, index, total
    );
}

std::string format_tui_needs_attention(
    std::size_t blocked_items, std::string_view last_blocked, Language language
) {
    const auto& catalog = catalog_for(language);
    return format_runtime(
        catalog.templates.tui_needs_attention_pattern,
        blocked_items,
        last_blocked
    );
}

std::string translate_confirmation_prompt(
    const ConfirmationRequest& request, Language language
) {
    const auto& catalog = catalog_for(language);
    const std::string_view prompt{request.prompt};
    if (const auto exact = lookup_entry(catalog.exact_prompts, prompt)) {
        return std::string{*exact};
    }
    for (const auto& rule : catalog.single_arg_prompts) {
        if (const auto arg = extract_single(prompt, rule.prefix, rule.suffix)) {
            return format_runtime(rule.pattern, *arg);
        }
    }
    return request.prompt;
}

std::string_view translate_file_action(
    events::FileAction action, Language language
) noexcept {
    const auto& catalog = catalog_for(language);
    if (const auto translated = lookup_enum(catalog.file_actions, action)) {
        return *translated;
    }
    return events::file_action_name(action);
}

std::string_view
translate_tui_view_title(TuiView view, Language language) noexcept {
    const auto& catalog = catalog_for(language);
    if (const auto translated = lookup_enum(catalog.tui_view_titles, view)) {
        return *translated;
    }
    if (const auto fallback =
            lookup_enum(kEnglishCatalog.tui_view_titles, view)) {
        return *fallback;
    }
    return "SYNC";
}

std::string_view
translate_sync_phase(events::SyncPhase phase, Language language) noexcept {
    const auto& catalog = catalog_for(language);
    if (const auto translated = lookup_enum(catalog.sync_phases, phase)) {
        return *translated;
    }
    return events::sync_phase_name(phase);
}

std::string_view
translate_sync_mode(sync::SyncMode mode, Language language) noexcept {
    const auto& catalog = catalog_for(language);
    if (const auto translated = lookup_enum(catalog.sync_modes, mode)) {
        return *translated;
    }
    return sync::sync_mode_name(mode);
}

} // namespace onedrive::cli::detail
