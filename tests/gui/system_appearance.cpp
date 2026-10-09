#include "system_appearance.hpp"

#include <QApplication>
#include <QDBusVariant>
#include <QMetaObject>
#include <QStyleHints>

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    onedrive::gui::SystemAppearance appearance;
    const auto deliver = [&](const QString& setting_namespace,
                             const QString& key,
                             const QVariant& value) {
        return QMetaObject::invokeMethod(
            &appearance,
            "setting_changed",
            Qt::DirectConnection,
            Q_ARG(QString, setting_namespace),
            Q_ARG(QString, key),
            Q_ARG(QDBusVariant, QDBusVariant{value})
        );
    };
    for (const auto id : {1U, 2U, 0U}) {
        const auto expected = id == 1   ? Qt::ColorScheme::Dark
                              : id == 2 ? Qt::ColorScheme::Light
                                        : Qt::ColorScheme::Unknown;
        const QVariant nested = QVariant::fromValue(
            QDBusVariant{QVariant::fromValue(QDBusVariant{QVariant{id}})}
        );
        if (onedrive::gui::desktop_color_scheme(QVariant{id}) != expected ||
            onedrive::gui::desktop_color_scheme(nested) != expected ||
            !deliver("org.freedesktop.appearance", "color-scheme", id)) {
            std::cerr << "desktop appearance value did not map to color scheme "
                      << id << '\n';
            return EXIT_FAILURE;
        }
    }
    for (const auto& invalid : {
             QVariant{3U},
             QVariant{-1},
             QVariant{"not-a-number"},
         }) {
        if (onedrive::gui::desktop_color_scheme(invalid) ||
            !deliver("org.freedesktop.appearance", "color-scheme", invalid) ||
            application.styleHints()->colorScheme() !=
                Qt::ColorScheme::Unknown) {
            std::cerr << "invalid desktop color scheme changed appearance\n";
            return EXIT_FAILURE;
        }
    }
    if (!deliver("another.namespace", "color-scheme", 1U) ||
        !deliver("org.freedesktop.appearance", "another-key", 1U) ||
        application.styleHints()->colorScheme() != Qt::ColorScheme::Unknown) {
        std::cerr << "unrelated desktop setting changed appearance\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
