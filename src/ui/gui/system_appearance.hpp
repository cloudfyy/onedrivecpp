#pragma once

#include <QObject>
#include <QVariant>

#include <QDBusVariant>
#include <QString>

#include <cstdint>

namespace onedrive::gui {

class SystemAppearance final : public QObject {
    Q_OBJECT

public:
    explicit SystemAppearance(QObject* parent = nullptr);

private Q_SLOTS:
    void setting_changed(
        const QString& setting_namespace,
        const QString& key,
        const QDBusVariant& value
    );

private:
    void apply_portal_color_scheme(const QVariant& value);

    std::uint64_t setting_change_generation_{};
};

} // namespace onedrive::gui
