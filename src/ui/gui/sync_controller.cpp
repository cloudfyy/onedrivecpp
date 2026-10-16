#include "sync_controller.hpp"

#include "configuration.hpp"
#include "app/preflight.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/synchronization.hpp"
#include "onedrive/app/monitoring.hpp"
#include "onedrive/app/queries.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/logging/logging.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace onedrive::gui {
namespace {

constexpr std::size_t maximum_pending_events = 500;

class ForwardingObserver final : public events::Observer {
public:
    explicit ForwardingObserver(
        std::function<void(const events::Event&)> forward
    )
        : forward_{std::move(forward)} {
    }

private:
    void on_event(const events::Event& event) const override {
        forward_(event);
    }

    std::function<void(const events::Event&)> forward_;
};

class RestoreLogger {
public:
    RestoreLogger()
        : previous_{spdlog::default_logger()} {
    }
    ~RestoreLogger() {
        // CLI logging sessions shut down spdlog; the GUI continues after a
        // task.
        if (previous_) {
            spdlog::set_default_logger(std::move(previous_));
        }
    }

private:
    std::shared_ptr<spdlog::logger> previous_;
};

bool is_progress(const events::Event& event) {
    return std::holds_alternative<events::DeltaProgressEvent>(event) ||
           std::holds_alternative<events::DownloadProgressEvent>(event) ||
           std::holds_alternative<events::UploadProgressEvent>(event);
}

} // namespace

SyncController::SyncController(
    SynchronizationFunction synchronize,
    QObject* parent,
    MonitoringFunction monitor
)
    : SyncController{CachedSynchronizationFunction{}, parent} {
    if (synchronize) {
        synchronize_ = [function = std::move(synchronize)](
                           config::Config config,
                           const events::Observer& observer,
                           std::stop_token token,
                           sync::PreviewCache*
                       ) {
            return function(std::move(config), observer, token);
        };
    }
    if (monitor) {
        monitor_ = std::move(monitor);
    }
}

SyncController::SyncController(
    CachedSynchronizationFunction synchronize, QObject* parent
)
    : QObject{parent},
      synchronize_{std::move(synchronize)} {
    monitor_ = [](config::Config config,
                  const events::Observer& observer,
                  std::stop_token token) {
        const app::ProductionRuntimeFactory production;
        const app::RuntimeFactory factory{util::borrowed_proxy, production};
        return app::monitor_account(
            std::move(config), factory, observer, false, token
        );
    };
    if (!synchronize_) {
        synchronize_ = [](config::Config config,
                          const events::Observer& observer,
                          std::stop_token token,
                          sync::PreviewCache* cache) {
            const app::ProductionRuntimeFactory production;
            const app::RuntimeFactory factory{util::borrowed_proxy, production};
            return app::synchronize_account(
                std::move(config), factory, observer, token, cache
            );
        };
    }
}

SyncController::~SyncController() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (blocked_worker_.joinable()) {
        blocked_worker_.join();
    }
}

void SyncController::refresh_blocked_items(
    const std::filesystem::path& config_file
) {
    if (blocked_refreshing_) {
        blocked_refresh_pending_ = config_file;
        return;
    }
    blocked_refreshing_ = true;
    try {
        blocked_worker_ = std::jthread{[this, config_file] {
            std::vector<storage::BlockedItem> items;
            QString error;
            try {
                bool missing = false;
                const auto config = load_configuration(config_file, missing);
                if (!missing) {
                    items = app::load_blocked_items(config);
                }
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            }
            QMetaObject::invokeMethod(
                this,
                [this, items = std::move(items), error] {
                    blocked_worker_.join();
                    blocked_refreshing_ = false;
                    if (blocked_refresh_pending_) {
                        const auto pending = std::exchange(
                            blocked_refresh_pending_, std::nullopt
                        );
                        refresh_blocked_items(*pending);
                        return;
                    }
                    Q_EMIT blocked_items_ready(items, error);
                },
                Qt::QueuedConnection
            );
        }};
    } catch (const std::exception& error) {
        blocked_refreshing_ = false;
        Q_EMIT blocked_items_ready({}, QString::fromUtf8(error.what()));
    }
}

bool SyncController::running() const noexcept {
    return running_;
}

void SyncController::cancel() {
    worker_.request_stop();
}

void SyncController::invalidate_preview() {
    if (running_) {
        throw std::logic_error{
            "Cannot invalidate preview metadata during synchronization."
        };
    }
    preview_cache_.clear();
}

void SyncController::start(
    const std::filesystem::path& config_file, bool preview
) {
    start_task(config_file, preview, false);
}

void SyncController::start_monitoring(
    const std::filesystem::path& config_file
) {
    start_task(config_file, false, true);
}

void SyncController::start_task(
    const std::filesystem::path& config_file, bool preview, bool monitoring
) {
    if (running_) {
        throw std::logic_error{
            tr("Synchronization is already running.").toStdString()
        };
    }
    {
        const std::scoped_lock lock{events_mutex_};
        task_totals_ = {};
        pending_summary_.reset();
    }
    running_ = true;
    try {
        worker_ = std::jthread{[this, config_file, preview, monitoring](
                                   std::stop_token token
                               ) {
            const RestoreLogger restore_logger;
            int result = exit_code::failure;
            QString error;
            bool effective_preview = preview;
            try {
                if (token.stop_requested()) {
                    result = exit_code::cancelled;
                } else {
                    bool missing = false;
                    std::string fingerprint;
                    auto config =
                        load_configuration(config_file, missing, &fingerprint);
                    bool invalidated = false;
                    if (config_file != preview_config_file_ ||
                        fingerprint != preview_config_fingerprint_ || missing) {
                        invalidated = preview_cache_.available();
                        preview_cache_.clear();
                    }
                    preview_config_file_ = config_file;
                    preview_config_fingerprint_ = std::move(fingerprint);
                    effective_preview = preview || config.dry_run;
                    config.dry_run = effective_preview;
                    config.force_large_delete = false;
                    if (monitoring) {
                        preview_cache_.clear();
                    }
                    set_file_history_limit(config.file_history_limit);
                    Q_EMIT started(effective_preview);
                    const ForwardingObserver observer{
                        [this](const events::Event& event) {
                            queue_event(event);
                        }
                    };
                    config.logging.message_sink =
                        [&observer](logging::Severity severity,
                                    std::string_view message) {
                            observer.message(
                                severity, "log", message
                            );
                        };
                    const logging::Session logging{config.logging};
                    const app::detail::RuntimePreflight preflight{
                        config,
                        monitoring ? app::detail::Operation::monitor
                                   : app::detail::Operation::synchronize,
                        &observer
                    };
                    if (invalidated) {
                        observer.message(
                            events::MessageKind::information,
                            "preview_configuration_changed",
                            "Saved configuration changed after preview; "
                            "fetching remote changes again..."
                        );
                    }
                    result = token.stop_requested() ? exit_code::cancelled
                             : monitoring
                                 ? monitor_(std::move(config), observer, token)
                                 : synchronize_(
                                       std::move(config),
                                       observer,
                                       token,
                                       &preview_cache_
                                   );
                }
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            }
            if (monitoring || !effective_preview ||
                (result != exit_code::success &&
                 result != exit_code::blocked) ||
                !error.isEmpty() || token.stop_requested()) {
                preview_cache_.clear();
            }
            QMetaObject::invokeMethod(
                this,
                [this, result, error = std::move(error), effective_preview] {
                    worker_.join();
                    drain_events();
                    running_ = false;
                    Q_EMIT finished(result, error, effective_preview);
                },
                Qt::QueuedConnection
            );
        }};
    } catch (...) {
        running_ = false;
        throw;
    }
}

void SyncController::queue_event(const events::Event& event) {
    const std::scoped_lock lock{events_mutex_};
    if (const auto* operation =
            std::get_if<events::FileOperationEvent>(&event)) {
        file_history_.add(*operation);
        pending_file_operations_.add(*operation);
        file_history_changed_ = true;
    } else if (const auto* summary =
                   std::get_if<events::SyncSummaryEvent>(&event)) {
        // Round statistics must survive overflow of the diagnostic event queue.
        task_totals_ = events::accumulate_sync_summary(task_totals_, *summary);
        pending_summary_ = *summary;
    } else if (is_progress(event) && !pending_events_.empty() &&
               pending_events_.back().index() == event.index()) {
        pending_events_.back() = event;
    } else {
        if (pending_events_.size() == maximum_pending_events) {
            pending_events_.pop_front();
            ++omitted_events_;
        }
        pending_events_.push_back(event);
    }
    schedule_delivery();
}

void SyncController::set_file_history_limit(std::size_t limit) {
    const std::scoped_lock lock{events_mutex_};
    if (file_history_.limit() == limit) {
        return;
    }
    file_history_.set_limit(limit);
    pending_file_operations_.set_limit(limit);
    file_history_changed_ = true;
    schedule_delivery();
}

void SyncController::schedule_delivery() {
    if (!delivery_pending_) {
        delivery_pending_ = true;
        QMetaObject::invokeMethod(
            this, [this] { drain_events(); }, Qt::QueuedConnection
        );
    }
}

void SyncController::drain_events() {
    std::vector<events::Event> batch;
    std::optional<events::SyncSummaryEvent> summary;
    events::SyncSummaryEvent totals;
    std::vector<events::FileOperationEvent> operations;
    std::size_t history_retained = 0;
    std::size_t history_omitted = 0;
    std::size_t history_limit = 0;
    bool history_changed = false;
    std::size_t omitted = 0;
    {
        const std::scoped_lock lock{events_mutex_};
        batch.assign(
            std::make_move_iterator(pending_events_.begin()),
            std::make_move_iterator(pending_events_.end())
        );
        pending_events_.clear();
        summary = std::exchange(pending_summary_, std::nullopt);
        totals = task_totals_;
        history_changed = std::exchange(file_history_changed_, false);
        operations.assign(
            pending_file_operations_.entries().begin(),
            pending_file_operations_.entries().end()
        );
        pending_file_operations_.clear();
        history_retained = file_history_.entries().size();
        history_omitted = file_history_.omitted();
        history_limit = file_history_.limit();
        omitted = std::exchange(omitted_events_, 0);
        delivery_pending_ = false;
    }
    if (!batch.empty() || omitted != 0) {
        Q_EMIT events_ready(batch, omitted);
    }
    if (summary) {
        Q_EMIT summary_ready(*summary, totals);
    }
    if (history_changed) {
        Q_EMIT file_operations_ready(
            operations, history_retained, history_omitted, history_limit
        );
    }
}

} // namespace onedrive::gui
