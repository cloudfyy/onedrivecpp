#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

namespace onedrive::gui {

using window_detail::number;

namespace {

constexpr std::size_t maximum_log_history = 500;

class LogBlockData final : public QTextBlockUserData {
public:
    explicit LogBlockData(std::size_t sequence)
        : sequence{sequence} {
    }
    std::size_t sequence;
};

} // namespace

void MainWindow::create_logs_page() {
    logs_page_ = new QWidget{tabs_};
    logs_page_->setObjectName("logsPage");
    auto* logs_layout = new QVBoxLayout{logs_page_};
    auto* log_actions = new QHBoxLayout;
    log_level_ = new QComboBox{logs_page_};
    log_level_->setObjectName("logLevelFilter");
    log_level_->addItem({}, -1);
    for (const auto kind :
         {events::MessageKind::trace,
          events::MessageKind::debug,
          events::MessageKind::information,
          events::MessageKind::warning,
          events::MessageKind::error,
          events::MessageKind::critical}) {
        log_level_->addItem({}, static_cast<int>(kind));
    }
    log_actions->addWidget(log_level_);
    log_autoscroll_ = new QCheckBox{logs_page_};
    log_autoscroll_->setObjectName("logAutoScroll");
    log_autoscroll_->setChecked(true);
    log_actions->addWidget(log_autoscroll_);
    log_actions->addStretch();
    copy_logs_button_ = new QPushButton{logs_page_};
    copy_logs_button_->setObjectName("copyLogsButton");
    clear_logs_button_ = new QPushButton{logs_page_};
    clear_logs_button_->setObjectName("clearLogsButton");
    log_actions->addWidget(copy_logs_button_);
    log_actions->addWidget(clear_logs_button_);
    logs_layout->addLayout(log_actions);
    log_messages_ = new QPlainTextEdit{logs_page_};
    log_messages_->setObjectName("logMessages");
    log_messages_->setReadOnly(true);
    log_messages_->setUndoRedoEnabled(false);
    logs_layout->addWidget(log_messages_);
    tabs_->addTab(logs_page_, tr("Logs"));

    connect(
        log_level_,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::render_logs
    );
    connect(log_autoscroll_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            auto* scroll = log_messages_->verticalScrollBar();
            scroll->setValue(scroll->maximum());
        }
    });
    connect(copy_logs_button_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(log_messages_->toPlainText());
    });
    connect(clear_logs_button_, &QPushButton::clicked, this, [this] {
        log_history_.clear();
        log_omitted_ = 0;
        render_logs();
    });
}

QString MainWindow::log_event_text(const events::Event& event) const {
    if (const auto* message = std::get_if<events::MessageEvent>(&event)) {
        if (message->event == "pending_delta_resumed") {
            return tr(
                "Restoring saved remote changes; checking only subsequent "
                "changes and rebuilding the local plan..."
            );
        }
        if (message->event == "pending_delta_invalidated") {
            return tr(
                "Saved remote changes no longer match the account, filters, "
                "hash policy, unsupported formats or applied cursor; fetching "
                "remote changes again..."
            );
        }
        if (message->event == "sync_filter_changed") {
            return tr(
                "Synchronization filters or unsupported-format rules changed; "
                "fetching the full remote state..."
            );
        }
        if (message->event == "pending_delta_hierarchy_changed") {
            return tr(
                "Remote directory changes or unidentified deletions may "
                "affect saved paths; fetching the full remote state..."
            );
        }
        return QString::fromStdString(message->text);
    } else if (const auto* section =
                   std::get_if<events::SectionEvent>(&event)) {
        auto text = QString::fromStdString(section->title);
        for (const auto& field : section->fields) {
            const auto label = field.key == "unsupported_items"
                                   ? tr("unsupported format items:")
                                   : QString::fromStdString(field.label);
            text += "\n" + label + " " + QString::fromStdString(field.value);
        }
        return text;
    } else if (const auto* summary =
                   std::get_if<events::DeltaSummaryEvent>(&event)) {
        return tr("Remote changes: %1 files, %2 folders, %3 deletions.")
            .arg(
                number(summary->summary.files),
                number(summary->summary.directories),
                number(summary->summary.deletions)
            );
    } else if (const auto* blocked =
                   std::get_if<events::BlockedItemEvent>(&event)) {
        if (sync::blocked_item_strategy(blocked->reason_code).traits().kind ==
            sync::BlockedPolicyKind::unsupported_format) {
            return tr("Unsupported format: %1 [%2]\n"
                      "Skipped; existing local files are preserved.")
                .arg(
                    QString::fromStdString(blocked->path),
                    QString::fromStdString(blocked->reason_code)
                );
        }
        return tr("Blocked: %1 [%2]\n%3")
            .arg(
                QString::fromStdString(blocked->path),
                QString::fromStdString(blocked->reason_code),
                QString::fromStdString(blocked->reason_message)
            );
    }
    throw std::logic_error{"non-message event in synchronization history"};
}

void MainWindow::append_log_event(
    LogOperation operation,
    const events::Event& event,
    const char* source,
    const QString& detail
) {
    const auto* message = std::get_if<events::MessageEvent>(&event);
    const auto kind = message ? message->kind
                      : std::holds_alternative<events::BlockedItemEvent>(event)
                          ? events::MessageKind::warning
                          : events::MessageKind::information;
    if (log_history_.size() == maximum_log_history) {
        log_history_.pop_front();
        ++log_omitted_;
    }
    log_history_.push_back(
        {next_log_sequence_++,
         QDateTime::currentDateTime(),
         operation,
         kind,
         event,
         source,
         detail}
    );
}

void MainWindow::log_message(
    LogOperation operation,
    events::MessageKind kind,
    const char* source,
    const QString& detail
) {
    append_log_event(
        operation, events::MessageEvent{kind, "gui", {}}, source, detail
    );
    render_logs();
}

void MainWindow::log_sync_result() {
    const auto operation =
        sync_preview_ ? LogOperation::preview : LogOperation::synchronization;
    switch (sync_phase_) {
    case SyncPhase::completed:
        log_message(
            operation,
            events::MessageKind::information,
            sync_preview_
                ? QT_TR_NOOP(
                      "Preview completed. No synchronized files were changed."
                  )
                : QT_TR_NOOP("Synchronization completed.")
        );
        break;
    case SyncPhase::cancelled:
        log_message(
            operation,
            events::MessageKind::information,
            sync_preview_ ? QT_TR_NOOP("Preview cancelled.")
                          : QT_TR_NOOP("Synchronization cancelled.")
        );
        break;
    case SyncPhase::blocked:
        log_message(
            operation,
            events::MessageKind::warning,
            sync_preview_
                ? QT_TR_NOOP("Preview incomplete: some items were blocked.")
                : QT_TR_NOOP(
                      "Synchronization incomplete: some items were blocked."
                  )
        );
        break;
    case SyncPhase::failed:
        if (!sync_error_.isEmpty()) {
            log_message(
                operation,
                events::MessageKind::error,
                sync_preview_ ? QT_TR_NOOP("Preview failed: %1")
                              : QT_TR_NOOP("Synchronization failed: %1"),
                sync_error_
            );
        } else {
            log_message(
                operation,
                events::MessageKind::error,
                sync_preview_
                    ? QT_TR_NOOP(
                          "Preview failed (exit code %1). See logs for details."
                      )
                    : QT_TR_NOOP(
                          "Synchronization failed (exit code %1). See details "
                          "in logs."
                      ),
                QString::number(sync_result_)
            );
        }
        break;
    default:
        throw std::logic_error{
            "synchronization result logged before completion"
        };
    }
}

void MainWindow::log_startup_errors() {
    if (state_.configuration_status == ConfigurationStatus::missing) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::warning,
            QT_TR_NOOP(
                "Configuration file is missing. Using defaults; complete setup "
                "in Settings."
            )
        );
    } else if (state_.configuration_status == ConfigurationStatus::invalid) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP(
                "Configuration could not be loaded:\n%1\n"
                "Import a valid TOML file, or repair this file and reload. "
                "The original file has not been changed."
            ),
            QString::fromStdString(state_.configuration_error)
        );
    }
    if (!state_.authentication_error.empty()) {
        log_message(
            LogOperation::account,
            events::MessageKind::error,
            QT_TR_NOOP("Authentication configuration needs attention:\n%1"),
            QString::fromStdString(state_.authentication_error)
        );
    }
    if (!state_.account_error.empty()) {
        log_message(
            LogOperation::account,
            events::MessageKind::error,
            QT_TR_NOOP(
                "Local account credentials could not be read:\n%1\n"
                "Repair the state directory or select another one in Settings, "
                "then reload."
            ),
            QString::fromStdString(state_.account_error)
        );
    }
    if (!state_.sync_error.empty()) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::warning,
            QT_TR_NOOP(
                "Sync directory needs attention before synchronization:\n%1"
            ),
            QString::fromStdString(state_.sync_error)
        );
    }
}

void MainWindow::render_logs() {
    QStringList messages;
    QList<std::optional<std::size_t>> sequences;
    if (log_omitted_ != 0) {
        messages << tr("Earlier events omitted: %1").arg(number(log_omitted_));
        sequences.push_back(std::nullopt);
    }
    const int filter = log_level_->currentData().toInt();
    for (const auto& entry : log_history_) {
        if (filter != -1 && static_cast<int>(entry.kind) < filter) {
            continue;
        }
        auto text =
            entry.source ? tr(entry.source) : log_event_text(entry.event);
        if (entry.source && !entry.detail.isNull()) {
            text = text.arg(entry.detail);
        }
        switch (entry.kind) {
        case events::MessageKind::trace:
            text = tr("Trace: %1").arg(text);
            break;
        case events::MessageKind::debug:
            text = tr("Debug: %1").arg(text);
            break;
        case events::MessageKind::information:
            text = tr("Info: %1").arg(text);
            break;
        case events::MessageKind::warning:
            text = tr("Warning: %1").arg(text);
            break;
        case events::MessageKind::error:
            text = tr("Error: %1").arg(text);
            break;
        case events::MessageKind::critical:
            text = tr("Critical: %1").arg(text);
            break;
        }
        QString operation;
        switch (entry.operation) {
        case LogOperation::application:
            operation = tr("Application");
            break;
        case LogOperation::account:
            operation = tr("Account");
            break;
        case LogOperation::configuration:
            operation = tr("Configuration");
            break;
        case LogOperation::preview:
            operation = tr("Preview");
            break;
        case LogOperation::synchronization:
            operation = tr("Sync");
            break;
        case LogOperation::monitoring:
            operation = tr("Monitoring");
            break;
        }
        messages << QString{"[%1] [%2] %3"}.arg(
            entry.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"), operation, text
        );
        sequences.push_back(entry.sequence);
    }
    const auto content = messages.join("\n");
    if (log_messages_->toPlainText() != content) {
        auto* scroll = log_messages_->verticalScrollBar();
        const auto position = scroll->value();
        const auto visible =
            log_messages_->cursorForPosition(QPoint{0, 0}).block();
        const auto line_offset = position - visible.firstLineNumber();
        auto header = visible;
        while (header.isValid() && !header.userData()) {
            header = header.previous();
        }
        const auto* data = header.isValid()
                               ? dynamic_cast<LogBlockData*>(header.userData())
                               : nullptr;
        const auto anchor = data ? std::optional{data->sequence} : std::nullopt;
        const auto block_offset = visible.blockNumber() - header.blockNumber();
        const auto horizontal = log_messages_->horizontalScrollBar()->value();
        log_messages_->clear();
        QTextCursor cursor{log_messages_->document()};
        cursor.beginEditBlock();
        QTextBlock restored;
        for (qsizetype index = 0; index < messages.size(); ++index) {
            if (index != 0) {
                cursor.insertBlock();
            }
            auto block = cursor.block();
            cursor.insertText(messages[index]);
            if (const auto sequence = sequences[index]) {
                block.setUserData(new LogBlockData{*sequence});
                if (anchor == sequence) {
                    restored = block;
                }
            }
        }
        cursor.endEditBlock();
        if (log_autoscroll_->isChecked()) {
            scroll->setValue(scroll->maximum());
        } else if (restored.isValid()) {
            // History eviction changes line numbers; follow the retained event.
            for (int offset = 0;
                 offset < block_offset && restored.next().isValid() &&
                 !restored.next().userData();
                 ++offset) {
                restored = restored.next();
            }
            log_messages_->setTextCursor(QTextCursor{restored});
            log_messages_->ensureCursorVisible();
            scroll->setValue(restored.firstLineNumber() + line_offset);
        } else {
            scroll->setValue(anchor ? 0 : position);
        }
        log_messages_->horizontalScrollBar()->setValue(horizontal);
    }
    copy_logs_button_->setEnabled(!content.isEmpty());
    clear_logs_button_->setEnabled(!log_history_.empty() || log_omitted_ != 0);
}

} // namespace onedrive::gui
