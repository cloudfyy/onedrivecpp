#include "ui/cli/backend_factory.hpp"
#include "ui/cli/message.hpp"

#include "onedrive/ui/cli/console.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace onedrive::cli::detail {
namespace {

class JsonConsoleBackend final : public ConsoleBackend {
public:
    JsonConsoleBackend(
        ConsoleOptions options,
        std::ostream& output,
        std::ostream& error
    )
        : options_{options},
          output_{output},
          error_{error} {}

    void emit(const ConsoleEvent& event) override {
        std::visit(
            [this](const auto& value) { render(value); },
            event
        );
    }

    bool confirm(const ConfirmationRequest&) override {
        throw std::runtime_error(
            "interactive confirmation is unavailable with --output=json; "
            "use --yes to confirm explicitly"
        );
    }

    OutputMode output_mode() const noexcept override {
        return OutputMode::json;
    }

    UiMode ui_mode() const noexcept override {
        return UiMode::console;
    }

private:
    void render(const MessageEvent& event) {
        if (message_suppressed(options_, event.kind)) {
            return;
        }
        std::ostream& stream =
            event.kind == MessageKind::error ? error_ : output_;
        stream << nlohmann::json{
            {"event", event.event},
            {"level", message_level(event.kind)},
            {"message", event.text},
        }.dump() << '\n';
    }

    void render(const SectionEvent& event) {
        if (options_.quiet) {
            return;
        }
        nlohmann::json values = nlohmann::json::object();
        for (const auto& field : event.fields) {
            values[field.key] = field.value;
        }
        output_ << nlohmann::json{
            {"event", event.event},
            {"values", std::move(values)},
        }.dump() << '\n';
    }

    void render(const DeltaProgressEvent& event) {
        if (options_.quiet) {
            return;
        }
        output_ << nlohmann::json{
            {"event", "delta_progress"},
            {"pages", event.pages},
            {"items", event.items},
            {
                "completed",
                event.state == util::ProgressState::completed
            },
        }.dump() << '\n';
    }

    void render(const DeltaSummaryEvent& event) {
        if (options_.quiet) {
            return;
        }
        const auto& summary = event.summary;
        output_ << nlohmann::json{
            {"event", "delta_summary"},
            {"pages", summary.pages},
            {"scanned_items", summary.scanned_items},
            {"unique_changes", summary.unique_changes},
            {"files", summary.files},
            {"directories", summary.directories},
            {"deletions", summary.deletions},
        }.dump() << '\n';
    }

    void render(const BlockedItemEvent& event) {
        error_ << nlohmann::json{
            {"event", "item_blocked"},
            {"level", "warning"},
            {"path", event.path},
            {"reason_code", event.reason_code},
            {"message", event.reason_message},
        }.dump() << '\n';
    }

    void render(const DownloadProgressEvent& event) {
        if (options_.quiet) {
            return;
        }
        const auto percentage = download_progress_percentage(
            event.completed_files,
            event.file_count,
            event.downloaded,
            event.total,
            event.state
        );
        output_ << nlohmann::json{
            {"event", "download_progress"},
            {"completed_files", event.completed_files},
            {"file_count", event.file_count},
            {"downloaded_bytes", event.downloaded},
            {"total_bytes", event.total},
            {"percentage", percentage},
            {
                "completed",
                event.state == util::ProgressState::completed
            },
            {
                "bytes_per_second",
                event.metrics.bytes_per_second
            },
            {
                "estimated_seconds_remaining",
                event.metrics.estimated_seconds_remaining.has_value() ?
                    nlohmann::json(
                        *event.metrics.estimated_seconds_remaining
                    ) :
                    nlohmann::json(nullptr)
            },
            {
                "elapsed_milliseconds",
                event.metrics.elapsed_milliseconds
            },
        }.dump() << '\n';
    }

    void render(const EndDownloadProgressEvent&) {}

    void render(const OperationStateEvent&) {}

    ConsoleOptions options_;
    std::ostream& output_;
    std::ostream& error_;
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_json_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
) {
    return std::make_unique<JsonConsoleBackend>(
        options, output, error
    );
}

}  // namespace onedrive::cli::detail
