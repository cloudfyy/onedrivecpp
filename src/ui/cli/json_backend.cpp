#include "ui/cli/backend_factory.hpp"
#include "ui/cli/message.hpp"

#include "onedrive/ui/cli/console.hpp"
#include "onedrive/ui/common/file_history.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace onedrive::cli::detail {
namespace {

class JsonConsoleBackend final : public ConsoleBackend {
public:
    JsonConsoleBackend(
        ConsoleOptions options, std::ostream& output, std::ostream& error
    )
        : options_{options},
          language_{resolve_language(options.language)},
          output_{output},
          error_{error},
          file_history_{options.file_history_limit} {
    }

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

    Language language() const noexcept override {
        return language_;
    }

private:
    void render(const events::FileOperationEvent& event) {
        file_history_.add(event);
        if (options_.quiet) {
            return;
        }
        output_ << nlohmann::json{
            {"event", "file_operation"},
            {"completed_at", events::format_file_operation_time(event.completed_at)},
            {"completed_at_unix_milliseconds",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 event.completed_at.time_since_epoch()
             ).count()},
            {"action", events::file_action_name(event.action)},
            {"path", event.path},
            {"previous_path", event.previous_path},
            {"drive_id", event.drive_id},
            {"local_path", event.local_path},
            {"directory", event.directory},
            {"recovered", event.recovered},
            {"history_retained", file_history_.entries().size()},
            {"history_omitted", file_history_.omitted()},
        }.dump() << '\n' << std::flush;
    }
    void render(const MessageEvent& event) {
        if (message_suppressed(options_, event.kind)) {
            return;
        }
        std::ostream& stream =
            event.kind == MessageKind::error ||
                    event.kind == MessageKind::critical
                ? error_ : output_;
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
        render_transfer(
            event, event.downloaded, "download_progress", "downloaded_bytes"
        );
    }

    void render(const UploadProgressEvent& event) {
        render_transfer(
            event,
            event.uploaded,
            "upload_progress",
            "uploaded_bytes",
            event.total != 0 || event.file_count != 0 ||
                event.state == util::ProgressState::completed
        );
    }

    template <typename Progress>
    void render_transfer(
        const Progress& event,
        std::uint64_t bytes,
        std::string_view event_name,
        std::string_view bytes_key,
        bool measurable = true
    ) {
        if (options_.quiet) {
            return;
        }
        const auto percentage = download_progress_percentage(
            event.completed_files,
            event.file_count,
            bytes,
            event.total,
            event.state
        );
        output_ << nlohmann::json{
            {"event", event_name},
            {"completed_files", event.completed_files},
            {"file_count", event.file_count},
            {std::string{bytes_key}, bytes},
            {"total_bytes", event.total},
            {"percentage", measurable ? nlohmann::json(percentage)
                                     : nlohmann::json(nullptr)},
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
    void render(const EndUploadProgressEvent&) {
    }
    void render(const SyncPhaseEvent& event) {
        if (!options_.quiet) {
            output_ << nlohmann::json{
                {"event", "sync_phase"}, {"phase", events::sync_phase_name(event.phase)}
            }.dump() << '\n';
        }
    }
    void render(const SyncSummaryEvent&) {
    }
    void render(const events::SyncContextEvent& event) {
        if (!options_.quiet) {
            output_ << nlohmann::json{
                {"event", "sync_context"},
                {"direction", sync::sync_mode_name(event.direction)},
                {"preview", event.preview},
            }.dump() << '\n';
        }
    }

    ConsoleOptions options_;
    Language language_{Language::english};
    std::ostream& output_;
    std::ostream& error_;
    events::FileHistory file_history_;
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
