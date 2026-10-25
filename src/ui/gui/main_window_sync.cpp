#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "localization.hpp"
#include "onedrive/exit_code.hpp"
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <utility>

namespace onedrive::gui {

using window_detail::data_size;
using window_detail::duration;
using window_detail::number;
using window_detail::short_detail;
using window_detail::sync_result_text;

void MainWindow::connect_sync_controller() {
    connect(preview_button_, &QPushButton::clicked, this, [this] {
        start_sync(true);
    });
    connect(sync_button_, &QPushButton::clicked, this, [this] {
        start_sync(false);
    });
    connect(
        monitor_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::start_monitoring
    );
    connect(
        &monitor_,
        &MonitorController::changed,
        this,
        &MainWindow::monitor_changed
    );
    connect(
        &monitor_,
        &MonitorController::acknowledged,
        this,
        [this](
            monitor::ControlCommand command, const QString& acknowledgement
        ) {
            if (command == monitor::ControlCommand::stop) {
                sync_stopping_ = true;
                sync_phase_ = SyncPhase::stopping;
            }
            log_message(
                LogOperation::monitoring,
                events::MessageKind::information,
                acknowledgement == "merged"
                    ? QT_TR_NOOP(
                          "Monitor request merged; this does not mean "
                          "synchronization or shutdown is complete."
                      )
                    : QT_TR_NOOP(
                          "Monitor request accepted; this does not mean "
                          "synchronization or shutdown is complete."
                      )
            );
            refresh_actions();
            render_sync();
        }
    );
    connect(
        &monitor_,
        &MonitorController::request_failed,
        this,
        [this](const QString& error) {
            if (!synchronization_.running()) {
                sync_stopping_ = false;
            }
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitor request failed: %1"),
                error
            );
            refresh_actions();
            render_sync();
        }
    );
    monitor_timer_ = new QTimer{this};
    monitor_timer_->setInterval(1000);
    connect(
        monitor_timer_, &QTimer::timeout, this, &MainWindow::refresh_monitor
    );
    monitor_timer_->start();
    connect(
        stop_sync_button_, &QPushButton::clicked, this, &MainWindow::stop_sync
    );
    connect(
        &synchronization_,
        &SyncController::started,
        this,
        [this](bool preview) {
            sync_preview_ = preview;
            if (!sync_stopping_) {
                sync_phase_ = SyncPhase::working;
            }
            if (!sync_monitoring_) {
                log_message(
                    preview ? LogOperation::preview
                            : LogOperation::synchronization,
                    events::MessageKind::information,
                    preview ? QT_TR_NOOP("Previewing changes...")
                            : QT_TR_NOOP("Synchronizing...")
                );
            }
            render_sync();
        },
        Qt::QueuedConnection
    );
    connect(
        &synchronization_,
        &SyncController::events_ready,
        this,
        &MainWindow::receive_sync_events
    );
    connect(
        &synchronization_,
        &SyncController::summary_ready,
        this,
        [this](
            const events::SyncSummaryEvent& summary,
            const events::SyncSummaryEvent& totals
        ) {
            sync_summary_ = summary;
            if (sync_monitoring_) {
                monitor_totals_ = totals;
            }
            render_sync();
        }
    );
    connect(
        &synchronization_,
        &SyncController::finished,
        this,
        [this](int result, const QString& error, bool preview) {
            sync_preview_ = preview;
            sync_result_ = result;
            sync_error_ = error;
            sync_stopping_ = false;
            if (!sync_monitoring_) {
                sync_finished_at_ = QDateTime::currentDateTime();
            }
            sync_phase_ = !error.isEmpty()               ? SyncPhase::failed
                          : result == exit_code::success ? SyncPhase::completed
                          : result == exit_code::cancelled
                              ? SyncPhase::cancelled
                          : result == exit_code::blocked ? SyncPhase::blocked
                                                         : SyncPhase::failed;
            if (sync_monitoring_) {
                log_message(
                    LogOperation::monitoring,
                    error.isEmpty() && (result == exit_code::success ||
                                        result == exit_code::cancelled)
                        ? events::MessageKind::information
                        : events::MessageKind::error,
                    !error.isEmpty() ? QT_TR_NOOP("Monitoring failed: %1")
                    : result == exit_code::success ||
                            result == exit_code::cancelled
                        ? QT_TR_NOOP("Monitoring stopped.")
                        : QT_TR_NOOP("Monitoring failed (exit code %1)."),
                    !error.isEmpty() ? error
                    : result == exit_code::success ||
                            result == exit_code::cancelled
                        ? QString{}
                        : QString::number(result)
                );
                monitor_.set_state_directory(state_.state_directory);
                refresh_monitor();
            } else {
                log_sync_result();
            }
            refresh_actions();
            render_sync();
            if (!closing_) {
                refresh_blocked_items();
            }
            if (closing_) {
                close();
            }
        }
    );
}

void MainWindow::start_sync(bool preview) {
    if (!preview && monitor_.status()) {
        const auto& reply = *monitor_.status();
        if (reply.state == "waiting" && !reply.pending && !sync_stopping_ &&
            monitor_.error().isEmpty() && !monitor_.busy()) {
            request_monitor(monitor::ControlCommand::trigger, reply.target());
        }
        return;
    }
    if (monitor_busy()) {
        return;
    }
    start_task(preview, false);
}

void MainWindow::start_monitoring() {
    if (monitor_busy()) {
        return;
    }
    start_task(false, true);
}

void MainWindow::start_task(bool preview, bool monitoring) {
    if (login_.running() || synchronization_.running() || monitor_busy() ||
        monitor_.uncertain() || !state_.can_synchronize()) {
        return;
    }
    sync_preview_ = preview;
    sync_monitoring_ = monitoring;
    had_monitor_ = false;
    attached_monitor_instance_.clear();
    sync_stopping_ = false;
    sync_phase_ = SyncPhase::preparing;
    sync_error_.clear();
    sync_delta_.reset();
    sync_download_.reset();
    sync_upload_.reset();
    sync_stage_.reset();
    sync_summary_.reset();
    monitor_totals_ =
        monitoring ? std::optional{events::SyncSummaryEvent{}} : std::nullopt;
    sync_finished_at_ = {};
    log_message(
        monitoring ? LogOperation::monitoring
        : preview  ? LogOperation::preview
                   : LogOperation::synchronization,
        events::MessageKind::information,
        monitoring ? QT_TR_NOOP("Starting GUI-owned monitoring...")
        : preview  ? QT_TR_NOOP("Preparing preview...")
                   : QT_TR_NOOP("Preparing synchronization...")
    );
    try {
        if (monitoring) {
            synchronization_.start_monitoring(state_.config_file);
        } else {
            synchronization_.start(state_.config_file, preview);
        }
    } catch (const std::exception& error) {
        sync_phase_ = SyncPhase::failed;
        sync_error_ = QString::fromUtf8(error.what());
        sync_result_ = exit_code::failure;
        if (monitoring) {
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitoring failed: %1"),
                sync_error_
            );
        } else {
            log_sync_result();
        }
    }
    refresh_actions();
    render_sync();
}

void MainWindow::stop_sync() {
    if (!synchronization_.running() && monitor_.status()) {
        const auto reply = *monitor_.status();
        if (reply.state == "stopping" || sync_stopping_ || monitor_.busy()) {
            return;
        }
        const auto confirmed = QMessageBox::question(
            this,
            tr("Stop external Monitor?"),
            tr("Stop Monitor PID %1 for %2? It is not owned by this window. "
               "Closing the window alone will leave it running.")
                .arg(
                    QString::number(reply.pid),
                    QString::fromStdString(reply.sync_directory.string())
                ),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (confirmed == QMessageBox::Yes) {
            request_monitor(monitor::ControlCommand::stop, reply.target());
        }
        return;
    }
    if (!synchronization_.running() || sync_stopping_) {
        return;
    }
    sync_stopping_ = true;
    sync_phase_ = SyncPhase::stopping;
    log_message(
        sync_monitoring_ ? LogOperation::monitoring
        : sync_preview_  ? LogOperation::preview
                         : LogOperation::synchronization,
        events::MessageKind::information,
        QT_TR_NOOP("Stopping; waiting for the task to finish...")
    );
    synchronization_.cancel();
    refresh_actions();
    render_sync();
}

bool MainWindow::monitor_busy() const {
    return monitor_.status().has_value() || monitor_.owner().has_value() ||
           (monitor_.uncertain() && !monitor_.runtime_free());
}

void MainWindow::refresh_monitor() {
    if (!closing_ && !login_.running() &&
        state_.configuration_status == ConfigurationStatus::valid) {
        monitor_.refresh();
    }
}

void MainWindow::request_monitor(
    monitor::ControlCommand command, monitor::ControlTarget target
) {
    try {
        monitor_.request(command, std::move(target));
    } catch (const std::exception& error) {
        log_message(
            LogOperation::monitoring,
            events::MessageKind::error,
            QT_TR_NOOP("Monitor request failed: %1"),
            QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::monitor_changed() {
    const bool owns_monitor = synchronization_.running() && sync_monitoring_;
    const bool attached = monitor_.status().has_value();
    const bool monitor_owner =
        monitor_.owner() && monitor_.owner()->operation == "monitor";
    if (!owns_monitor && !synchronization_.running() &&
        (attached || monitor_owner)) {
        if (!had_monitor_ || (attached && attached_monitor_instance_ !=
                                              monitor_.status()->instance_id)) {
            sync_delta_.reset();
            sync_download_.reset();
            sync_upload_.reset();
            sync_stage_.reset();
            sync_summary_.reset();
            monitor_totals_.reset();
            sync_finished_at_ = {};
            if (attached) {
                log_message(
                    LogOperation::monitoring,
                    events::MessageKind::information,
                    QT_TR_NOOP(
                        "Connected to an external Monitor. Closing this window "
                        "will not stop it."
                    )
                );
            }
        }
        sync_monitoring_ = true;
        sync_preview_ = false;
        if (attached) {
            attached_monitor_instance_ = monitor_.status()->instance_id;
            const auto& reply = *monitor_.status();
            sync_phase_ = reply.state == "waiting"    ? SyncPhase::waiting
                          : reply.state == "stopping" ? SyncPhase::stopping
                          : reply.state == "starting" ? SyncPhase::preparing
                                                      : SyncPhase::working;
        } else {
            sync_phase_ =
                sync_stopping_ ? SyncPhase::stopping : SyncPhase::preparing;
        }
    }
    if (had_monitor_ && !owns_monitor && !monitor_busy() &&
        !synchronization_.running()) {
        attached_monitor_instance_.clear();
        sync_phase_ = SyncPhase::completed;
        sync_stopping_ = false;
        log_message(
            LogOperation::monitoring,
            events::MessageKind::information,
            QT_TR_NOOP("Monitor stopped; runtime resources are available.")
        );
    }
    had_monitor_ = ((attached || monitor_owner) && !owns_monitor) ||
                   (had_monitor_ && monitor_busy());
    if (!owns_monitor && !synchronization_.running() &&
        !monitor_.error().isEmpty()) {
        sync_phase_ = SyncPhase::monitor_unavailable;
    }
    if (last_monitor_error_ != monitor_.error()) {
        last_monitor_error_ = monitor_.error();
        if (!last_monitor_error_.isEmpty()) {
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitor status unavailable: %1"),
                last_monitor_error_
            );
        }
    }
    refresh_actions();
    render_sync();
}

void MainWindow::receive_sync_events(
    const std::vector<events::Event>& batch, std::size_t omitted
) {
    log_omitted_ += omitted;
    for (const auto& event : batch) {
        // This marks a terminal newline, not completion of a download batch.
        if (std::holds_alternative<events::EndDownloadProgressEvent>(event) ||
            std::holds_alternative<events::EndUploadProgressEvent>(event)) {
            continue;
        }
        if (const auto* phase = std::get_if<events::SyncPhaseEvent>(&event)) {
            sync_stage_ = phase->phase;
            if (!sync_stopping_) {
                switch (phase->phase) {
                case events::SyncPhase::preparing:
                    if (sync_monitoring_) {
                        sync_delta_.reset();
                        sync_download_.reset();
                        sync_upload_.reset();
                    }
                    sync_phase_ = SyncPhase::preparing;
                    break;
                case events::SyncPhase::scanning:
                    if (!sync_delta_) {
                        sync_delta_ = events::DeltaProgressEvent{
                            0, 0, util::ProgressState::ongoing
                        };
                    }
                    sync_phase_ = SyncPhase::scanning;
                    break;
                case events::SyncPhase::scanning_local:
                    sync_phase_ = SyncPhase::scanning_local;
                    break;
                case events::SyncPhase::downloading:
                    sync_phase_ = SyncPhase::transferring;
                    break;
                case events::SyncPhase::uploading:
                    sync_phase_ = SyncPhase::uploading;
                    break;
                case events::SyncPhase::waiting:
                    if (sync_monitoring_) {
                        sync_phase_ = SyncPhase::waiting;
                        sync_finished_at_ = QDateTime::currentDateTime();
                    }
                    break;
                default:
                    sync_phase_ = SyncPhase::working;
                    break;
                }
            }
            continue;
        }
        if (const auto* context =
                std::get_if<events::SyncContextEvent>(&event)) {
            state_.sync_mode = context->direction;
            sync_preview_ = context->preview;
            continue;
        }
        if (const auto* upload =
                std::get_if<events::UploadProgressEvent>(&event)) {
            sync_upload_ = *upload;
            if (!sync_stopping_) {
                sync_phase_ = upload->state == util::ProgressState::ongoing
                                  ? SyncPhase::uploading
                                  : SyncPhase::working;
            }
            continue;
        }
        if (const auto* delta =
                std::get_if<events::DeltaProgressEvent>(&event)) {
            sync_delta_ = *delta;
            if (!sync_stopping_) {
                if (delta->state == util::ProgressState::ongoing) {
                    sync_download_.reset();
                    sync_phase_ = SyncPhase::scanning;
                } else {
                    sync_phase_ = SyncPhase::working;
                }
            }
        } else if (const auto* download =
                       std::get_if<events::DownloadProgressEvent>(&event)) {
            sync_download_ = *download;
            if (!sync_stopping_) {
                sync_phase_ = download->state == util::ProgressState::ongoing
                                  ? SyncPhase::transferring
                                  : SyncPhase::working;
            }
        } else if (const auto* state =
                       std::get_if<events::OperationStateEvent>(&event)) {
            if (!sync_stopping_ &&
                state->operation == events::OperationKind::synchronization &&
                state->state == events::OperationState::syncing) {
                sync_phase_ = SyncPhase::working;
            }
        } else {
            if (std::holds_alternative<events::DeltaSummaryEvent>(event) &&
                sync_phase_ == SyncPhase::scanning && !sync_stopping_) {
                sync_phase_ = SyncPhase::working;
            }
            append_log_event(
                sync_monitoring_ ? LogOperation::monitoring
                : sync_preview_  ? LogOperation::preview
                                 : LogOperation::synchronization,
                event
            );
        }
    }
    render_sync();
    refresh_actions();
    render_logs();
    if (std::ranges::any_of(batch, [](const auto& event) {
            if (std::holds_alternative<events::BlockedItemEvent>(event)) {
                return true;
            }
            const auto* message = std::get_if<events::MessageEvent>(&event);
            return message && message->kind == events::MessageKind::warning;
        })) {
        refresh_blocked_items();
    }
}

void MainWindow::render_sync() {
    const auto locale = locale_for_language(
        localization_ ? localization_->language() : Language::automatic
    );
    render_monitor_status(locale);
    render_sync_status(locale);
    const int percentage = render_download_status(locale);
    const bool measurable_upload = render_upload_progress(locale);
    render_sync_summary(locale);
    const bool show_progress = render_download_progress(percentage);
    render_progress_hint(show_progress, measurable_upload);
}

void MainWindow::render_monitor_status(const QLocale& locale) {
    const auto direction = state_.sync_mode == sync::SyncMode::upload_only
                               ? tr("Upload only")
                           : state_.sync_mode == sync::SyncMode::download_only
                               ? tr("Download only")
                               : tr("Bidirectional");
    const bool external =
        !(synchronization_.running() && sync_monitoring_) &&
        (!attached_monitor_instance_.empty() ||
         (monitor_.owner() && monitor_.owner()->operation == "monitor"));
    sync_context_->setText(
        external ? tr("Direction: not available from external Monitor; mode: "
                      "Automatic monitoring")
        : sync_monitoring_
            ? tr("Direction: %1; mode: Automatic monitoring").arg(direction)
            : tr("Direction: %1; mode: One-time").arg(direction)
    );
    QString monitor_text =
        monitor_.uncertain()
            ? tr("Monitor status unavailable. The task may still be running.")
            : tr("Monitor: not running.");
    if (monitor_.status()) {
        const auto& reply = *monitor_.status();
        monitor_text =
            tr("%1 Monitor: PID %2; directory: %3; completed rounds: %4; "
               "pending request: %5; last result: %6.")
                .arg(
                    external ? tr("External") : tr("GUI-owned"),
                    locale.toString(static_cast<qlonglong>(reply.pid)),
                    QString::fromStdString(reply.sync_directory.string()),
                    locale.toString(
                        static_cast<qulonglong>(reply.completed_rounds)
                    ),
                    reply.pending ? tr("Yes") : tr("No"),
                    reply.last_result ? sync_result_text(*reply.last_result)
                                      : tr("None")
                );
        if (external) {
            monitor_text +=
                tr(" External Monitor provides status snapshots, not live "
                   "transfer progress.");
        }
    } else if (monitor_.owner()) {
        monitor_text =
            tr("Runtime resources are busy: PID %1; operation: %2. Waiting for "
               "startup or cleanup; controls are unavailable.")
                .arg(
                    QString::number(monitor_.owner()->pid),
                    QString::fromStdString(monitor_.owner()->operation)
                );
    } else if (synchronization_.running() && sync_monitoring_) {
        monitor_text = tr("Starting GUI-owned monitoring...");
    }
    if (!monitor_.error().isEmpty()) {
        monitor_text += " " + tr("Monitor status unavailable: %1")
                                  .arg(short_detail(monitor_.error()));
    }
    monitor_status_->setText(monitor_text);
}

void MainWindow::render_sync_status(const QLocale& locale) {
    QString status;
    switch (sync_phase_) {
    case SyncPhase::idle:
        status = state_.can_synchronize()
                     ? tr("Ready. Tasks use saved settings.")
                     : tr("Sign in and save valid settings to synchronize.");
        break;
    case SyncPhase::preparing:
        status = sync_preview_ ? tr("Preparing preview...")
                               : tr("Preparing synchronization...");
        break;
    case SyncPhase::working:
        status = sync_preview_ ? tr("Previewing changes...")
                               : tr("Synchronizing...");
        if (!sync_preview_ && sync_stage_ == events::SyncPhase::applying) {
            status = tr("Applying changes...");
        }
        break;
    case SyncPhase::scanning:
        if (sync_delta_) {
            status =
                tr("Scanning remote changes: %1 pages, %2 items.")
                    .arg(
                        locale.toString(
                            static_cast<qulonglong>(sync_delta_->pages)
                        ),
                        locale.toString(
                            static_cast<qulonglong>(sync_delta_->items)
                        )
                    );
        } else {
            status = tr("Scanning remote changes...");
        }
        break;
    case SyncPhase::scanning_local:
        status = tr("Scanning local changes...");
        break;
    case SyncPhase::transferring:
        status = tr("Downloading files...");
        break;
    case SyncPhase::uploading:
        status = tr("Uploading files...");
        break;
    case SyncPhase::waiting:
        status = tr("Monitoring; waiting for changes.");
        if (monitor_.status() && monitor_.status()->last_result &&
            *monitor_.status()->last_result != exit_code::success) {
            status +=
                *monitor_.status()->last_result == exit_code::blocked
                    ? " " +
                          tr("Last round incomplete: some items were blocked.")
                    : " " + tr("Last round: %1. Monitoring continues.")
                                .arg(sync_result_text(
                                    *monitor_.status()->last_result
                                ));
        }
        break;
    case SyncPhase::monitor_unavailable:
        status =
            tr("Monitor status unavailable. The task may still be running.");
        break;
    case SyncPhase::stopping:
        status = tr("Stopping; waiting for the task to finish...");
        break;
    case SyncPhase::completed:
        status =
            sync_monitoring_ ? tr("Monitoring stopped.")
            : sync_preview_
                ? tr("Preview completed. No synchronized files were changed.")
                : tr("Synchronization completed.");
        break;
    case SyncPhase::cancelled:
        status = sync_monitoring_ ? tr("Monitoring stopped.")
                 : sync_preview_  ? tr("Preview cancelled.")
                                  : tr("Synchronization cancelled.");
        break;
    case SyncPhase::blocked:
        status =
            sync_preview_
                ? tr("Preview incomplete: some items were blocked.")
                : tr("Synchronization incomplete: some items were blocked.");
        break;
    case SyncPhase::failed:
        if (sync_monitoring_) {
            status =
                !sync_error_.isEmpty()
                    ? tr("Monitoring failed: %1").arg(short_detail(sync_error_))
                    : tr("Monitoring failed (exit code %1).").arg(sync_result_);
            break;
        }
        if (!sync_error_.isEmpty()) {
            status = (sync_preview_ ? tr("Preview failed: %1")
                                    : tr("Synchronization failed: %1"))
                         .arg(short_detail(sync_error_));
        } else {
            status =
                (sync_preview_
                     ? tr("Preview failed (exit code %1). See logs for "
                          "details.")
                     : tr("Synchronization failed (exit code %1). See details "
                          "in logs."))
                    .arg(sync_result_);
        }
        break;
    }
    sync_status_->setText(status);
    overview_sync_status_->setText(status);
}

int MainWindow::render_download_status(const QLocale& locale) {
    QString transfer;
    int percentage = 0;
    if (sync_download_) {
        const auto& download = *sync_download_;
        percentage = static_cast<int>(events::download_progress_percentage(
            download.completed_files,
            download.file_count,
            download.downloaded,
            download.total,
            download.state
        ));
        transfer = tr("Downloads: %1 / %2 files; %3 / %4; %5/s")
                       .arg(
                           locale.toString(
                               static_cast<qulonglong>(download.completed_files)
                           ),
                           locale.toString(
                               static_cast<qulonglong>(download.file_count)
                           ),
                           data_size(download.downloaded, locale),
                           data_size(download.total, locale),
                           data_size(download.metrics.bytes_per_second, locale)
                       );
        if (download.metrics.estimated_seconds_remaining &&
            synchronization_.running() &&
            sync_phase_ == SyncPhase::transferring) {
            transfer +=
                tr("; about %1 remaining")
                    .arg(duration(
                        *download.metrics.estimated_seconds_remaining, locale
                    ));
        }
    }
    transfer_status_->setText(transfer);
    transfer_status_->setVisible(!transfer.isEmpty());
    return percentage;
}

bool MainWindow::render_download_progress(int percentage) {
    const bool measurable_download =
        sync_download_ &&
        (sync_download_->total != 0 || sync_download_->file_count != 0 ||
         sync_download_->state == util::ProgressState::completed);
    const bool show_download =
        measurable_download &&
        (sync_phase_ == SyncPhase::transferring ||
         sync_phase_ == SyncPhase::stopping ||
         sync_phase_ == SyncPhase::cancelled ||
         sync_phase_ == SyncPhase::blocked || sync_phase_ == SyncPhase::failed);
    const bool show_progress =
        (!sync_monitoring_ && sync_phase_ == SyncPhase::completed) ||
        show_download;
    sync_progress_->setRange(0, 100);
    sync_progress_->setValue(
        sync_phase_ == SyncPhase::completed ? 100 : percentage
    );
    sync_progress_->setFormat("%p%");
    sync_progress_->setAccessibleName(
        show_download ? tr("Download progress") : tr("Synchronization progress")
    );
    sync_progress_->setVisible(show_progress);
    return show_progress;
}

bool MainWindow::render_upload_progress(const QLocale& locale) {
    QString upload_text;
    const bool measurable_upload =
        sync_upload_ &&
        (sync_upload_->total != 0 || sync_upload_->file_count != 0 ||
         sync_upload_->state == util::ProgressState::completed);
    if (sync_upload_) {
        const auto& upload = *sync_upload_;
        upload_text =
            tr("Uploads: %1 / %2 files; %3 / %4; %5/s")
                .arg(
                    locale.toString(
                        static_cast<qulonglong>(upload.completed_files)
                    ),
                    locale.toString(static_cast<qulonglong>(upload.file_count)),
                    data_size(upload.uploaded, locale),
                    data_size(upload.total, locale),
                    data_size(upload.metrics.bytes_per_second, locale)
                );
        if (upload.metrics.estimated_seconds_remaining &&
            synchronization_.running() && sync_phase_ == SyncPhase::uploading) {
            upload_text +=
                tr("; about %1 remaining")
                    .arg(duration(
                        *upload.metrics.estimated_seconds_remaining, locale
                    ));
        }
        upload_progress_->setRange(0, 100);
        upload_progress_->setValue(
            static_cast<int>(events::download_progress_percentage(
                upload.completed_files,
                upload.file_count,
                upload.uploaded,
                upload.total,
                upload.state
            ))
        );
    }
    upload_progress_->setFormat("%p%");
    upload_progress_->setAccessibleName(tr("Upload progress"));
    upload_progress_->setVisible(
        measurable_upload &&
        !(sync_monitoring_ && (sync_phase_ == SyncPhase::waiting ||
                               sync_phase_ == SyncPhase::completed))
    );
    upload_status_->setText(upload_text);
    upload_status_->setVisible(!upload_text.isEmpty());
    return measurable_upload;
}

void MainWindow::render_sync_summary(const QLocale& locale) {
    QString totals;
    if (monitor_totals_) {
        totals = tr("Monitor session totals: %1 downloaded, %2 uploaded, "
                    "%3 moved, %4 deleted.")
                     .arg(
                         number(monitor_totals_->downloaded),
                         number(monitor_totals_->uploaded),
                         number(monitor_totals_->moved),
                         number(monitor_totals_->deleted)
                     );
    }
    monitor_totals_status_->setText(totals);
    monitor_totals_status_->setVisible(!totals.isEmpty());
    QString summary;
    if (sync_summary_) {
        summary =
            (sync_monitoring_
                 ? tr("Last round totals: %1 downloaded, %2 uploaded, "
                      "%3 moved, %4 deleted, %5 blocked.")
                 : tr("Round totals: %1 downloaded, %2 uploaded, %3 moved, "
                      "%4 deleted, %5 blocked."))
                .arg(
                    number(sync_summary_->downloaded),
                    number(sync_summary_->uploaded),
                    number(sync_summary_->moved),
                    number(sync_summary_->deleted),
                    number(sync_summary_->blocked)
                );
    }
    if (sync_finished_at_.isValid()) {
        summary +=
            tr(" Last finished: %1.")
                .arg(locale.toString(sync_finished_at_, QLocale::ShortFormat));
    }
    sync_summary_status_->setText(summary);
    sync_summary_status_->setVisible(!summary.isEmpty());
}

void MainWindow::render_progress_hint(
    bool show_progress, bool measurable_upload
) {
    QString hint;
    if (synchronization_.running() && sync_phase_ != SyncPhase::waiting &&
        !show_progress &&
        !(sync_phase_ == SyncPhase::uploading && measurable_upload)) {
        if (sync_phase_ == SyncPhase::scanning) {
            hint = tr(
                "Remote totals are unknown; a percentage cannot be calculated."
            );
        } else if (sync_phase_ == SyncPhase::transferring) {
            hint =
                tr("Download totals are unknown; a percentage cannot be "
                   "calculated.");
        } else if (sync_phase_ == SyncPhase::uploading && !measurable_upload) {
            hint = tr(
                "Upload totals are unknown; a percentage cannot be calculated."
            );
        } else {
            hint = tr(
                "This phase has no percentage progress. See logs for details."
            );
        }
    }
    sync_progress_hint_->setText(hint);
    sync_progress_hint_->setVisible(!hint.isEmpty());
}

} // namespace onedrive::gui
