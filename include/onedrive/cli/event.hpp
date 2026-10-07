#pragma once

#include "onedrive/util/progress.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace onedrive::cli {

enum class ColorMode {
    automatic,
    always,
    never,
};

enum class OutputMode {
    text,
    json,
};

enum class UiMode {
    automatic,
    console,
    tui,
};

enum class TuiTheme {
    hacker,
    ocean,
    amber,
    synthwave,
};

enum class TuiView {
    auth,
    doctor,
    sync,
    download,
    monitor,
};

enum class MessageKind {
    information,
    success,
    warning,
    error,
};

struct ConsoleOptions {
    ColorMode color{ColorMode::automatic};
    OutputMode output{OutputMode::text};
    UiMode ui{UiMode::console};
    TuiTheme theme{TuiTheme::hacker};
    TuiView view{TuiView::sync};
    bool quiet{false};
};

struct Field {
    std::string label;
    std::string key;
    std::string value;
};

struct DownloadProgressMetrics {
    std::uint64_t bytes_per_second{0};
    std::optional<std::uint64_t> estimated_seconds_remaining;
    std::uint64_t elapsed_milliseconds{0};
};

struct DeltaSummary {
    std::size_t pages{0};
    std::size_t scanned_items{0};
    std::size_t unique_changes{0};
    std::size_t files{0};
    std::size_t directories{0};
    std::size_t deletions{0};
};

struct MessageEvent {
    MessageKind kind;
    std::string event;
    std::string text;
};

struct SectionEvent {
    std::string event;
    std::string title;
    std::vector<Field> fields;
};

struct DeltaProgressEvent {
    std::size_t pages;
    std::size_t items;
    util::ProgressState state;
};

struct DeltaSummaryEvent {
    DeltaSummary summary;
};

struct BlockedItemEvent {
    std::string path;
    std::string reason_code;
    std::string reason_message;
};

struct DownloadProgressEvent {
    std::size_t completed_files;
    std::size_t file_count;
    std::uint64_t downloaded;
    std::uint64_t total;
    util::ProgressState state;
    DownloadProgressMetrics metrics;
};

struct EndDownloadProgressEvent {};

using ConsoleEvent = std::variant<
    MessageEvent,
    SectionEvent,
    DeltaProgressEvent,
    DeltaSummaryEvent,
    BlockedItemEvent,
    DownloadProgressEvent,
    EndDownloadProgressEvent
>;

struct ConfirmationRequest {
    std::string event;
    std::string prompt;
    std::string expected;
};

}  // namespace onedrive::cli
