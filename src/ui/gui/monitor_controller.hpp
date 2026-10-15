#pragma once

#include "onedrive/monitor/control.hpp"
#include "app/lock.hpp"

#include <QObject>
#include <QString>
#include <thread>

namespace onedrive::gui {

class MonitorController final : public QObject {
    Q_OBJECT

public:
    explicit MonitorController(QObject* parent = nullptr);
    ~MonitorController() override;
    void set_state_directory(const std::filesystem::path& state_directory);
    void refresh();
    void request(
        monitor::ControlCommand command, monitor::ControlTarget expected_target
    );
    [[nodiscard]] const std::optional<monitor::ControlReply>&
    status() const noexcept;
    [[nodiscard]] const QString& error() const noexcept;
    [[nodiscard]] bool requesting() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool uncertain() const noexcept;
    [[nodiscard]] bool runtime_free() const noexcept;
    [[nodiscard]] const std::optional<app::detail::LockOwner>&
    owner() const noexcept;

Q_SIGNALS:
    void changed();
    void acknowledged(monitor::ControlCommand command, QString acknowledgement);
    void request_failed(QString error);

private:
    void query(
        monitor::ControlCommand command,
        std::optional<monitor::ControlTarget> expected_target
    );
    std::filesystem::path state_directory_;
    std::optional<monitor::ControlReply> status_;
    std::optional<app::detail::LockOwner> owner_;
    QString error_;
    std::jthread worker_;
    bool busy_{false};
    bool requesting_{false};
    bool uncertain_{false};
    bool runtime_free_{false};
    std::uint64_t generation_{0};
};

} // namespace onedrive::gui
