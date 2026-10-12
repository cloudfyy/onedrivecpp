#include "sync_controller.hpp"

#include "configuration.hpp"
#include "app/preflight.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/synchronization.hpp"
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
           std::holds_alternative<events::DownloadProgressEvent>(event);
}

} // namespace

SyncController::SyncController(
    SynchronizationFunction synchronize, QObject* parent
)
    : QObject{parent},
      synchronize_{std::move(synchronize)} {
    if (!synchronize_) {
        synchronize_ = [](config::Config config,
                          const events::Observer& observer,
                          std::stop_token token) {
            const app::ProductionRuntimeFactory production;
            const app::RuntimeFactory factory{util::borrowed_proxy, production};
            return app::synchronize_account(
                std::move(config), factory, observer, token
            );
        };
    }
}

SyncController::~SyncController() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool SyncController::running() const noexcept {
    return running_;
}

void SyncController::cancel() {
    worker_.request_stop();
}

void SyncController::start(
    const std::filesystem::path& config_file, bool preview
) {
    if (running_) {
        throw std::logic_error{
            tr("Synchronization is already running.").toStdString()
        };
    }
    running_ = true;
    try {
        worker_ = std::jthread{[this,
                                config_file,
                                preview](std::stop_token token) {
            const RestoreLogger restore_logger;
            int result = exit_code::failure;
            QString error;
            bool effective_preview = preview;
            try {
                if (token.stop_requested()) {
                    result = exit_code::cancelled;
                } else {
                    bool missing = false;
                    auto config = load_configuration(config_file, missing);
                    effective_preview = preview || config.dry_run;
                    config.dry_run = effective_preview;
                    config.force_large_delete = false;
                    Q_EMIT started(effective_preview);
                    const logging::Session logging{config.logging};
                    const app::detail::RuntimePreflight preflight{
                        config, app::detail::Operation::synchronize
                    };
                    const ForwardingObserver observer{
                        [this](const events::Event& event) {
                            queue_event(event);
                        }
                    };
                    result =
                        token.stop_requested()
                            ? exit_code::cancelled
                            : synchronize_(std::move(config), observer, token);
                }
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
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
    if (is_progress(event) && !pending_events_.empty() &&
        pending_events_.back().index() == event.index()) {
        pending_events_.back() = event;
    } else {
        if (pending_events_.size() == maximum_pending_events) {
            pending_events_.pop_front();
            ++omitted_events_;
        }
        pending_events_.push_back(event);
    }
    if (!delivery_pending_) {
        delivery_pending_ = true;
        QMetaObject::invokeMethod(
            this, [this] { drain_events(); }, Qt::QueuedConnection
        );
    }
}

void SyncController::drain_events() {
    std::vector<events::Event> batch;
    std::size_t omitted = 0;
    {
        const std::scoped_lock lock{events_mutex_};
        batch.assign(
            std::make_move_iterator(pending_events_.begin()),
            std::make_move_iterator(pending_events_.end())
        );
        pending_events_.clear();
        omitted = std::exchange(omitted_events_, 0);
        delivery_pending_ = false;
    }
    if (!batch.empty() || omitted != 0) {
        Q_EMIT events_ready(batch, omitted);
    }
}

} // namespace onedrive::gui
