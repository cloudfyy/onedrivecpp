#pragma once

#include "onedrive/config/config.hpp"

#include <QWidget>
#include <functional>
#include <vector>

class QFormLayout;
class QLabel;
class QTabWidget;
class QToolButton;
class QCheckBox;

namespace onedrive::gui {

class SettingsEditor final : public QWidget {
    Q_OBJECT
public:
    explicit SettingsEditor(QWidget* parent = nullptr);
    void load(const config::Config& config);
    void retranslate();
    [[nodiscard]] std::vector<config::SettingPatch> patches() const;

private:
    enum class Kind {
        text,
        number,
        choice,
        boolean,
        optional_path,
        optional_text,
        list,
        override_text,
        override_list
    };
    struct Field {
        std::string table;
        std::string key;
        const char* label;
        const char* tip;
        Kind kind;
        QWidget* widget;
        QLabel* label_widget;
        QWidget* container;
        std::function<QString(const config::Config&)> read;
        QString baseline;
        std::vector<std::pair<const char*, const char*>> choices;
        QCheckBox* override = nullptr;
        std::function<bool(const config::Config&)> present;
        bool baseline_present = false;
    };
    void
    add(QFormLayout* form,
        const char* table,
        const char* key,
        const char* label,
        const char* tip,
        Kind kind,
        std::function<QString(const config::Config&)> read,
        std::vector<std::pair<const char*, const char*>> choices = {},
        bool directory = false,
        std::function<bool(const config::Config&)> present = {});
    [[nodiscard]] static QString value(const Field& field);
    QTabWidget* tabs_;
    std::vector<Field> fields_;
    std::vector<std::pair<QLabel*, const char*>> notes_;
    std::vector<std::pair<QToolButton*, const char*>> advanced_;
};

} // namespace onedrive::gui
