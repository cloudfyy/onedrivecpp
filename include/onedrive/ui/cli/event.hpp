#pragma once

#include "onedrive/config/console.hpp"
#include "onedrive/ui/common/event.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace onedrive::cli {

using config::ColorMode;

enum class OutputMode {
    text,
    json,
};

using config::UiMode;

using config::TuiTheme;

enum class TuiView {
    auth,
    health,
    status,
    drives,
    shared,
    sites,
    quota,
    storage,
    partials,
    files,
    verify,
    config,
    sync,
    download,
    watch,
};

using events::MessageKind;

struct ConsoleOptions {
    ColorMode color{ColorMode::automatic};
    OutputMode output{OutputMode::text};
    UiMode ui{UiMode::console};
    TuiTheme theme{TuiTheme::hacker};
    TuiView view{TuiView::sync};
    bool quiet{false};
};

using events::BlockedItemEvent;
using events::DeltaProgressEvent;
using events::DeltaSummary;
using events::DeltaSummaryEvent;
using events::DownloadProgressEvent;
using events::DownloadProgressMetrics;
using events::EndDownloadProgressEvent;
using events::Field;
using events::MessageEvent;
using events::SectionEvent;
using ConsoleEvent = events::Event;

struct ConfirmationRequest {
    std::string event;
    std::string prompt;
    std::string expected;
};

}  // namespace onedrive::cli
