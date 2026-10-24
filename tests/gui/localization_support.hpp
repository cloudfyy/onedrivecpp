#pragma once

#include "localization.hpp"
#include "main_window.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/version.hpp"
#include "support/common.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTabBar>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <stdexcept>
#include <string_view>
#include "gui/support.hpp"

namespace onedrive::test::gui_localization {

namespace app = onedrive::app;
using namespace onedrive;
using gui_support::require;
using gui_support::wait_until;

inline QString
expected(bool chinese, const char* english, const char* translation) {
    return QString::fromUtf8(chinese ? translation : english);
}

inline bool has_label(const QWidget& widget, const QString& text) {
    for (const auto* label : widget.findChildren<QLabel*>()) {
        if (label->text() == text) {
            return true;
        }
    }
    return false;
}

inline QPushButton* button(QWidget& widget, const QString& text) {
    for (auto* candidate : widget.findChildren<QPushButton*>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    throw std::runtime_error{"missing translated button"};
}

inline std::filesystem::path
write_config(const std::filesystem::path& directory) {
    const auto file = directory / "config.toml";
    test::write_file(
        file,
        "config_version = 2\n[sync]\ndata_directory = \"" +
            (directory / "sync-%2-中文").string() +
            "\"\n[state]\ndirectory = \"" +
            (directory / "state-%3-中文").string() +
            "\"\n[auth]\napplication_id = \"test-client\"\n"
    );
    return file;
}

} // namespace onedrive::test::gui_localization
