#include "monitor_controller.hpp"

#include <stdexcept>

namespace onedrive::gui {

MonitorController::MonitorController(QObject* parent)
    : QObject{parent} {
}

MonitorController::~MonitorController() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MonitorController::set_state_directory(
    const std::filesystem::path& directory
) {
    state_directory_ = directory;
    ++generation_;
    status_.reset();
    owner_.reset();
    error_.clear();
    uncertain_ = false;
    runtime_free_ = false;
    Q_EMIT changed();
}

const std::optional<monitor::ControlReply>&
MonitorController::status() const noexcept {
    return status_;
}

const QString& MonitorController::error() const noexcept {
    return error_;
}
bool MonitorController::requesting() const noexcept {
    return requesting_;
}
bool MonitorController::busy() const noexcept {
    return busy_;
}
bool MonitorController::uncertain() const noexcept {
    return uncertain_;
}
bool MonitorController::runtime_free() const noexcept {
    return runtime_free_;
}
const std::optional<app::detail::LockOwner>&
MonitorController::owner() const noexcept {
    return owner_;
}

void MonitorController::refresh() {
    if (!busy_ && !state_directory_.empty()) {
        try {
            query(monitor::ControlCommand::status, std::nullopt);
        } catch (const std::exception& error) {
            error_ = QString::fromUtf8(error.what());
            uncertain_ = true;
            runtime_free_ = false;
            Q_EMIT changed();
        }
    }
}

void MonitorController::request(
    monitor::ControlCommand command, monitor::ControlTarget expected_target
) {
    if (busy_) {
        throw std::logic_error(
            "A Monitor status or control request is already running."
        );
    }
    if (state_directory_.empty()) {
        throw std::logic_error(
            "No state directory is selected for Monitor control."
        );
    }
    query(command, std::move(expected_target));
}

void MonitorController::query(
    monitor::ControlCommand command,
    std::optional<monitor::ControlTarget> expected_target
) {
    busy_ = true;
    requesting_ = command != monitor::ControlCommand::status;
    const auto generation = generation_;
    const auto directory = state_directory_;
    try {
        worker_ = std::jthread{
            [this, directory, generation, command, expected_target] {
                std::optional<monitor::ControlReply> reply;
                std::optional<app::detail::LockOwner> owner;
                QString error;
                bool uncertain = false;
                bool runtime_free = false;
                try {
                    if (command == monitor::ControlCommand::status) {
                        try {
                            reply = monitor::find_control(directory);
                        } catch (const std::system_error& failure) {
                            uncertain =
                                failure.code() != std::errc::connection_refused;
                            error = QString::fromUtf8(failure.what());
                        } catch (const std::exception& failure) {
                            uncertain = true;
                            error = QString::fromUtf8(failure.what());
                        }
                        if (!reply) {
                            owner =
                                app::detail::inspect_runtime_owner(directory);
                            runtime_free = !owner;
                        }
                    } else {
                        reply = monitor::request_control(
                            directory, command, expected_target
                        );
                    }
                } catch (const std::exception& failure) {
                    error = QString::fromUtf8(failure.what());
                    uncertain = true;
                }
                QMetaObject::invokeMethod(
                    this,
                    [this,
                     generation,
                     command,
                     reply = std::move(reply),
                     owner = std::move(owner),
                     error,
                     uncertain,
                     runtime_free] {
                        worker_.join();
                        busy_ = false;
                        requesting_ = false;
                        if (generation != generation_) {
                            Q_EMIT changed();
                            refresh();
                            return;
                        }
                        error_ = error;
                        uncertain_ = uncertain;
                        runtime_free_ = runtime_free;
                        if (error.isEmpty() ||
                            command == monitor::ControlCommand::status) {
                            status_ = reply;
                            owner_ = owner;
                        }
                        Q_EMIT changed();
                        if (command != monitor::ControlCommand::status) {
                            if (!error.isEmpty()) {
                                Q_EMIT request_failed(error);
                            } else {
                                Q_EMIT acknowledged(
                                    command,
                                    QString::fromStdString(
                                        reply->acknowledgement
                                    )
                                );
                            }
                        }
                    },
                    Qt::QueuedConnection
                );
            }
        };
        Q_EMIT changed();
    } catch (...) {
        busy_ = false;
        requesting_ = false;
        throw;
    }
}

} // namespace onedrive::gui
