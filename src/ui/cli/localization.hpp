#pragma once

#include "onedrive/ui/cli/event.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace onedrive::cli::detail {

struct CliFormatterLabels {
    std::string_view usage;
    std::string_view options_upper;
    std::string_view options;
    std::string_view subcommands_upper;
    std::string_view subcommands;
    std::string_view positionals;
    std::string_view required;
};

[[nodiscard]] CliFormatterLabels
cli_formatter_labels(Language language) noexcept;
[[nodiscard]] std::size_t display_width(std::string_view text) noexcept;
[[nodiscard]] std::string_view
translate_text(std::string_view english, Language language) noexcept;
[[nodiscard]] std::string translate_section_title(
    std::string_view event, std::string_view title, Language language
);
[[nodiscard]] std::string translate_field_label(
    std::string_view key, std::string_view label, Language language
);
[[nodiscard]] std::string translate_tui_section_title(
    std::string_view event, std::string_view title, Language language
);
[[nodiscard]] std::string translate_tui_field_label(
    std::string_view key, std::string_view label, Language language
);
[[nodiscard]] std::string translate_field_value(
    std::string_view key, std::string_view value, Language language
);
[[nodiscard]] std::string
translate_message(const MessageEvent& event, Language language);
[[nodiscard]] std::optional<std::string>
translate_tui_message(const MessageEvent& event, Language language);
[[nodiscard]] std::string translate_blocked_reason(
    std::string_view reason_code,
    std::string_view reason_message,
    Language language
);
[[nodiscard]] std::string format_tui_blocked_item(
    const BlockedItemEvent& event, Language language
);
[[nodiscard]] std::string format_delta_summary(
    const events::DeltaSummary& summary, Language language
);
[[nodiscard]] std::string format_blocked_item_line(
    const BlockedItemEvent& event, Language language
);
[[nodiscard]] std::string translate_confirmation_prompt(
    const ConfirmationRequest& request, Language language
);
[[nodiscard]] std::string_view translate_file_action(
    events::FileAction action, Language language
) noexcept;
[[nodiscard]] std::string_view
translate_tui_view_title(TuiView view, Language language) noexcept;
[[nodiscard]] std::string_view
translate_sync_phase(events::SyncPhase phase, Language language) noexcept;
[[nodiscard]] std::string_view
translate_sync_mode(sync::SyncMode mode, Language language) noexcept;

[[nodiscard]] std::string format_file_operation_line(
    std::string_view formatted_path,
    events::FileAction action,
    bool directory,
    bool recovered,
    std::string_view local_path,
    std::string_view completed_at,
    Language language
);
[[nodiscard]] std::string format_file_history_summary(
    std::size_t retained,
    std::size_t limit,
    std::size_t omitted,
    std::string_view drives_csv,
    Language language
);
[[nodiscard]] std::string format_text_transfer_progress(
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
);
[[nodiscard]] std::string format_text_sync_phase_line(
    events::SyncPhase phase, Language language
);
[[nodiscard]] std::string format_text_sync_context_line(
    sync::SyncMode direction, bool preview, Language language
);
[[nodiscard]] std::string format_tui_cloud_status(
    std::size_t scanned_items,
    std::optional<std::size_t> changes_found,
    Language language
);
[[nodiscard]] std::string format_tui_transfer_unknown_compact(
    std::string_view title, Language language
);
[[nodiscard]] std::string format_tui_transfer_heading(
    std::string_view title,
    std::size_t completed_files,
    std::size_t file_count,
    Language language
);
[[nodiscard]] std::string format_tui_transfer_details(
    unsigned int percentage,
    std::string_view transferred_bytes,
    std::string_view total_bytes,
    std::string_view rate_bytes,
    bool completed,
    std::optional<std::string_view> duration,
    Language language
);
[[nodiscard]] std::string format_tui_sync_context(
    sync::SyncMode direction, bool monitoring, bool preview, Language language
);
[[nodiscard]] std::string format_tui_sync_phase(
    bool monitoring, events::SyncPhase phase, Language language
);
[[nodiscard]] std::string format_tui_sync_summary(
    const events::SyncSummaryEvent& summary, Language language
);
[[nodiscard]] std::string format_tui_file_history_heading(
    std::size_t retained, std::size_t omitted, Language language
);
[[nodiscard]] std::string format_tui_paged_section_title(
    std::string_view title,
    TuiView view,
    std::size_t index,
    std::size_t total,
    Language language
);
[[nodiscard]] std::string format_tui_partial_heading(
    std::size_t index, std::size_t total, Language language
);
[[nodiscard]] std::string format_tui_needs_attention(
    std::size_t blocked_items, std::string_view last_blocked, Language language
);

} // namespace onedrive::cli::detail
