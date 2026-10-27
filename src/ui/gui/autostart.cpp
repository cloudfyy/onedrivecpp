#include "autostart.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <stdexcept>

namespace onedrive::gui {
namespace {

[[noreturn]] void unreadable_entry(const QString& path) {
    throw std::runtime_error{
        ("Cannot read the autostart entry: " + path).toStdString()
    };
}

QString quoted_executable() {
    auto executable = QCoreApplication::applicationFilePath();
    executable.replace('%', "%%");
    // Exec quoting is decoded after Desktop Entry string escaping.
    for (const auto character :
         {QChar{'\\'}, QChar{'"'}, QChar{'`'}, QChar{'$'}}) {
        executable.replace(character, QString{"\\"} + character);
    }
    executable.replace('\\', "\\\\");
    executable.replace('\n', "\\n");
    executable.replace('\r', "\\r");
    executable.replace('\t', "\\t");
    return '"' + executable + '"';
}

} // namespace

QString autostart_file_path() {
    const auto config =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (config.isEmpty()) {
        throw std::runtime_error{
            "Cannot locate the user configuration directory."
        };
    }
    return QDir{config}.filePath("autostart/onedrive-cpp-gui.desktop");
}

bool autostart_enabled() {
    const auto path = autostart_file_path();
    if (!QFileInfo::exists(path)) {
        return false;
    }
    QFile entry{path};
    if (!entry.open(QIODevice::ReadOnly)) {
        unreadable_entry(path);
    }
    QString type;
    QString executable;
    QString hidden{"false"};
    bool desktop_entry = false;
    while (!entry.atEnd()) {
        const auto line = QString::fromUtf8(entry.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        if (line.startsWith('[')) {
            if (!line.endsWith(']')) {
                unreadable_entry(path);
            }
            desktop_entry = line == "[Desktop Entry]";
            continue;
        }
        const auto separator = line.indexOf('=');
        if (separator <= 0) {
            unreadable_entry(path);
        }
        if (!desktop_entry) {
            continue;
        }
        const auto key = line.left(separator).trimmed();
        const auto value = line.mid(separator + 1).trimmed();
        if (key == "Type") {
            type = value;
        } else if (key == "Exec") {
            executable = value;
        } else if (key == "Hidden") {
            hidden = value;
        }
    }
    if (entry.error() != QFileDevice::NoError || type != "Application" ||
        executable.isEmpty() || (hidden != "true" && hidden != "false")) {
        unreadable_entry(path);
    }
    return hidden == "false";
}

void set_autostart_enabled(bool enabled) {
    const auto path = autostart_file_path();
    if (!QDir{}.mkpath(QFileInfo{path}.absolutePath())) {
        throw std::runtime_error{
            ("Cannot create the autostart directory: " + path).toStdString()
        };
    }
    const auto contents =
        QString{"[Desktop Entry]\n"
                "Type=Application\n"
                "Name=OneDrive Client\n"
                "Exec=%1 --start-in-tray\n"
                "Icon=onedrive-cpp\n"
                "Terminal=false\n"
                "Hidden=%2\n"}
            .arg(quoted_executable(), enabled ? "false" : "true")
            .toUtf8();
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit()) {
        throw std::runtime_error{("Cannot save the autostart entry: " + path +
                                  ": " + file.errorString())
                                     .toStdString()};
    }
}

} // namespace onedrive::gui
