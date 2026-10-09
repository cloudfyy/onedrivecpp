#include "system_appearance.hpp"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QStyleHints>

namespace onedrive::gui {
namespace {

constexpr auto portal_service = "org.freedesktop.portal.Desktop";
constexpr auto portal_path = "/org/freedesktop/portal/desktop";
constexpr auto portal_interface = "org.freedesktop.portal.Settings";
constexpr auto appearance_namespace = "org.freedesktop.appearance";
constexpr auto color_scheme_key = "color-scheme";

} // namespace

SystemAppearance::SystemAppearance(QObject* parent)
    : QObject{parent} {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCDebug(QLoggingCategory{"onedrive.gui.appearance"})
            << "Session D-Bus unavailable; using the Qt platform appearance";
        return;
    }

    if (!bus.connect(
            portal_service,
            portal_path,
            portal_interface,
            "SettingChanged",
            this,
            SLOT(setting_changed(QString, QString, QDBusVariant))
        )) {
        qCDebug(QLoggingCategory{"onedrive.gui.appearance"})
            << "Could not subscribe to desktop appearance changes";
    }

    auto request = QDBusMessage::createMethodCall(
        portal_service, portal_path, portal_interface, "Read"
    );
    request << QString::fromLatin1(appearance_namespace)
            << QString::fromLatin1(color_scheme_key);
    const auto request_generation = setting_change_generation_;
    auto* watcher = new QDBusPendingCallWatcher{bus.asyncCall(request), this};
    connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        this,
        [this, request_generation](auto* call) {
            const QDBusPendingReply<QDBusVariant> reply{*call};
            if (reply.isError()) {
                qCDebug(QLoggingCategory{"onedrive.gui.appearance"})
                    << "Desktop color-scheme setting unavailable:"
                    << reply.error().message();
                return;
            }
            if (request_generation == setting_change_generation_) {
                apply_portal_color_scheme(reply.value().variant());
            }
        }
    );
}

void SystemAppearance::setting_changed(
    const QString& setting_namespace,
    const QString& key,
    const QDBusVariant& value
) {
    if (setting_namespace == QLatin1String{appearance_namespace} &&
        key == QLatin1String{color_scheme_key}) {
        ++setting_change_generation_;
        apply_portal_color_scheme(value.variant());
    }
}

void SystemAppearance::apply_portal_color_scheme(const QVariant& value) {
    auto setting_value = value;
    while (setting_value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        setting_value = setting_value.value<QDBusVariant>().variant();
    }

    bool valid = false;
    const auto scheme_id = setting_value.toUInt(&valid);
    if (!valid || scheme_id > 2 || QApplication::instance() == nullptr) {
        qCDebug(QLoggingCategory{"onedrive.gui.appearance"})
            << "Ignoring invalid desktop color-scheme value:" << setting_value;
        return;
    }

    const auto scheme = scheme_id == 1   ? Qt::ColorScheme::Dark
                        : scheme_id == 2 ? Qt::ColorScheme::Light
                                         : Qt::ColorScheme::Unknown;
    QGuiApplication::styleHints()->setColorScheme(scheme);
}

} // namespace onedrive::gui
