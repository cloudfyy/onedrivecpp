#pragma once

#include "configuration.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "settings_editor.hpp"
#include "support/common.hpp"
#include "onedrive/util/sha256.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include "gui/support.hpp"

namespace onedrive::test::gui_settings {

using gui_support::require;
using gui_support::wait_until;

using namespace onedrive;

inline std::size_t backups(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().contains(".backup.")) {
            ++count;
        }
    }
    return count;
}

inline void text(QWidget& window, const char* name, const QString& value) {
    auto* field = window.findChild<QLineEdit*>(name);
    require(field != nullptr, "text field missing");
    field->setText(value);
}

inline void choice(QWidget& window, const char* name, const char* value) {
    auto* combo = window.findChild<QComboBox*>(name);
    require(
        combo != nullptr && combo->findData(QString::fromUtf8(value)) >= 0,
        "enum field missing"
    );
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(value)));
}

inline void list(QWidget& window, const char* name, const QString& value) {
    auto* field = window.findChild<QPlainTextEdit*>(name);
    require(field != nullptr, "list field missing");
    field->setPlainText(value);
}

inline void checked(QWidget& window, const char* name, bool value) {
    auto* field = window.findChild<QCheckBox*>(name);
    require(field != nullptr, "checkbox field missing");
    field->setChecked(value);
}

inline bool same_patches(
    const std::vector<config::SettingPatch>& left,
    const std::vector<config::SettingPatch>& right
) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].table != right[i].table || left[i].key != right[i].key ||
            left[i].value != right[i].value) {
            return false;
        }
    }
    return true;
}

inline QString save(gui::MainWindow& window) {
    QString message;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message = dialog->text();
            if (dialog->standardButtons().testFlag(QMessageBox::Save)) {
                dialog->button(QMessageBox::Save)->click();
            } else {
                dialog->accept();
            }
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("saveSettingsButton")->click();
    dialogs.stop();
    require(!message.isEmpty(), "save did not display a result");
    return message;
}

} // namespace onedrive::test::gui_settings
