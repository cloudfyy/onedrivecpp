#pragma once

#include "onedrive/ui/cli/event.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace onedrive::cli::detail {

[[nodiscard]] bool is_chinese(Language language) noexcept;
[[nodiscard]] std::size_t display_width(std::string_view text) noexcept;
[[nodiscard]] std::string_view
translate_text(std::string_view english, Language language) noexcept;
[[nodiscard]] std::string translate_section_title(
    std::string_view event, std::string_view title, Language language
);
[[nodiscard]] std::string translate_field_label(
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

} // namespace onedrive::cli::detail
