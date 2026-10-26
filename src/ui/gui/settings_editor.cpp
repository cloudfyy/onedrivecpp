#include "settings_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <stdexcept>
#include <algorithm>

namespace onedrive::gui {

SettingsEditor::SettingsEditor(QWidget* parent)
    : QWidget{parent},
      tabs_{new QTabWidget{this}} {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    tabs_->setObjectName("settingsTabs");
    layout->addWidget(tabs_);
    add_account_page();
    add_sync_page();
    add_transfer_page();
    add_network_page();
    add_monitor_page();
    add_diagnostics_page();
    retranslate();
}

QFormLayout* SettingsEditor::page() {
    auto* scroll = new QScrollArea{tabs_};
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget{scroll};
    auto* form = new QFormLayout{contents};
    scroll->setWidget(contents);
    tabs_->addTab(scroll, {});
    return form;
}

void SettingsEditor::note(QFormLayout* form, const char* text) {
    auto* label = new QLabel;
    label->setWordWrap(true);
    form->addRow(label);
    notes_.emplace_back(label, text);
}

QFormLayout* SettingsEditor::advanced(QFormLayout* form, const char* title) {
    auto* toggle = new QToolButton;
    toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    auto* contents = new QWidget;
    auto* advanced_form = new QFormLayout{contents};
    form->addRow(toggle);
    form->addRow(contents);
    contents->hide();
    connect(
        toggle,
        &QToolButton::toggled,
        contents,
        [contents, toggle](bool expanded) {
            contents->setVisible(expanded);
            toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        }
    );
    advanced_.emplace_back(toggle, title);
    return advanced_form;
}

void SettingsEditor::add(
    QFormLayout* form,
    const char* table,
    const char* key,
    const char* label,
    const char* tip,
    Kind kind,
    std::function<QString(const config::Config&)> read,
    std::vector<std::pair<const char*, const char*>> choices,
    bool directory,
    std::function<bool(const config::Config&)> present
) {
    QWidget* widget = nullptr;
    if (kind == Kind::choice) {
        auto* combo = new QComboBox;
        for (const auto& choice : choices) {
            combo->addItem(QString{}, QString::fromUtf8(choice.first));
        }
        connect(
            combo,
            &QComboBox::currentIndexChanged,
            this,
            [this](int) { Q_EMIT changed(); }
        );
        widget = combo;
    } else if (kind == Kind::boolean) {
        auto* checkbox = new QCheckBox;
        connect(checkbox, &QCheckBox::toggled, this, [this](bool) {
            Q_EMIT changed();
        });
        widget = checkbox;
    } else if (kind == Kind::list || kind == Kind::override_list) {
        auto* edit = new QPlainTextEdit;
        edit->setMaximumHeight(90);
        connect(edit, &QPlainTextEdit::textChanged, this, [this] {
            Q_EMIT changed();
        });
        widget = edit;
    } else {
        auto* edit = new QLineEdit;
        if (kind == Kind::number) {
            edit->setPlaceholderText("0–9223372036854775807");
        }
        connect(edit, &QLineEdit::textChanged, this, [this](const QString&) {
            Q_EMIT changed();
        });
        widget = edit;
    }
    widget->setObjectName(QString::fromUtf8(table) + "." + key);
    auto* label_widget = new QLabel;
    label_widget->setBuddy(widget);
    QWidget* container = widget;
    if (directory || kind == Kind::optional_path) {
        container = new QWidget;
        auto* layout = new QHBoxLayout{container};
        layout->setContentsMargins(0, 0, 0, 0);
        auto* browse = new QPushButton{tr("Browse…")};
        browse->setObjectName("browseButton");
        layout->addWidget(widget);
        layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, widget, directory] {
            auto* edit = qobject_cast<QLineEdit*>(widget);
            const auto selected =
                directory ? QFileDialog::getExistingDirectory(
                                this, tr("Select directory"), edit->text()
                            )
                          : QFileDialog::getOpenFileName(
                                this, tr("Select file"), edit->text()
                            );
            if (!selected.isEmpty()) {
                edit->setText(selected);
            }
        });
    }
    QCheckBox* override = nullptr;
    if (kind == Kind::override_text || kind == Kind::override_list) {
        container = new QWidget;
        auto* layout = new QVBoxLayout{container};
        layout->setContentsMargins(0, 0, 0, 0);
        override = new QCheckBox{tr("Override environment")};
        override->setObjectName(
            QString::fromUtf8(table) + "." + key + ".override"
        );
        layout->addWidget(override);
        layout->addWidget(widget);
        connect(override, &QCheckBox::toggled, widget, &QWidget::setEnabled);
        connect(override, &QCheckBox::toggled, this, [this](bool) {
            Q_EMIT changed();
        });
    }
    if (std::string_view{table} == "sync" &&
        std::string_view{key} == "drive_ids") {
        container = new QWidget;
        auto* layout = new QVBoxLayout{container};
        layout->setContentsMargins(0, 0, 0, 0);
        auto* browse = new QPushButton{tr("Browse SharePoint libraries…")};
        browse->setObjectName("browseSharePointButton");
        layout->addWidget(widget);
        layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            browse_sharepoint_libraries();
        });
    }
    form->addRow(label_widget, container);
    fields_.push_back(
        {table,
         key,
         label,
         tip,
         kind,
         widget,
         label_widget,
         container,
         std::move(read),
         {},
         std::move(choices),
         override,
         std::move(present),
         false}
    );
}

QString SettingsEditor::value(const Field& field) {
    if (field.kind == Kind::choice) {
        return qobject_cast<QComboBox*>(field.widget)->currentData().toString();
    }
    if (field.kind == Kind::boolean) {
        return qobject_cast<QCheckBox*>(field.widget)->isChecked() ? "true"
                                                                   : "false";
    }
    if (field.kind == Kind::list || field.kind == Kind::override_list) {
        return qobject_cast<QPlainTextEdit*>(field.widget)->toPlainText();
    }
    return qobject_cast<QLineEdit*>(field.widget)->text();
}

void SettingsEditor::load(const config::Config& config) {
    config_ = config;
    for (auto& field : fields_) {
        field.baseline = field.read(config);
        if (field.override) {
            field.baseline_present = field.present(config);
            field.override->setChecked(field.baseline_present);
            field.widget->setEnabled(field.baseline_present);
        }
        if (field.kind == Kind::choice) {
            auto* combo = qobject_cast<QComboBox*>(field.widget);
            combo->setCurrentIndex(combo->findData(field.baseline));
        } else if (field.kind == Kind::boolean) {
            qobject_cast<QCheckBox*>(field.widget)
                ->setChecked(field.baseline == "true");
        } else if (field.kind == Kind::list ||
                   field.kind == Kind::override_list) {
            qobject_cast<QPlainTextEdit*>(field.widget)
                ->setPlainText(field.baseline);
        } else {
            qobject_cast<QLineEdit*>(field.widget)->setText(field.baseline);
        }
    }
    Q_EMIT changed();
}

void SettingsEditor::retranslate() {
    tabs_->setTabText(0, tr("Account and directories"));
    tabs_->setTabText(1, tr("Sync rules"));
    tabs_->setTabText(2, tr("Transfers"));
    tabs_->setTabText(3, tr("Network"));
    tabs_->setTabText(4, tr("Monitor"));
    tabs_->setTabText(5, tr("Advanced and logs"));
    for (auto& field : fields_) {
        field.label_widget->setText(tr(field.label));
        const auto tip = tr(field.tip);
        for (auto* target :
             {field.widget,
              static_cast<QWidget*>(field.label_widget),
              field.container}) {
            target->setToolTip(tip);
            target->setAccessibleDescription(tip);
        }
        if (field.container != field.widget) {
            for (auto* companion :
                 field.container->findChildren<QAbstractButton*>()) {
                companion->setToolTip(tip);
                companion->setAccessibleDescription(tip);
            }
        }
        if (field.override) {
            field.override->setText(tr("Override environment"));
        }
        if (field.kind == Kind::choice) {
            auto* combo = qobject_cast<QComboBox*>(field.widget);
            for (std::size_t i = 0; i < field.choices.size(); ++i) {
                combo->setItemText(
                    static_cast<int>(i), tr(field.choices[i].second)
                );
            }
        }
    }
    for (const auto& [label, source] : notes_) {
        label->setText(tr(source));
    }
    for (const auto& [toggle, source] : advanced_) {
        toggle->setText(tr(source));
    }
    for (auto* button : findChildren<QPushButton*>("browseButton")) {
        button->setText(tr("Browse…"));
    }
    for (auto* button : findChildren<QPushButton*>("browseSharePointButton")) {
        button->setText(tr("Browse SharePoint libraries…"));
    }
}

void SettingsEditor::set_site_search_function(SiteSearchFunction search) {
    site_search_ = std::move(search);
}

bool SettingsEditor::has_unsaved_changes() const {
    return std::any_of(
        fields_.begin(), fields_.end(), [](const Field& field) {
            const bool present = !field.override || field.override->isChecked();
            if (field.override) {
                if (present != field.baseline_present) {
                    return true;
                }
                if (!present) {
                    return false;
                }
            }
            return value(field) != field.baseline;
        }
    );
}

std::vector<config::SettingPatch> SettingsEditor::patches() const {
    std::vector<config::SettingPatch> patches;
    const auto proxy_url =
        std::find_if(fields_.begin(), fields_.end(), [](const auto& f) {
            return f.table == "proxy" && f.key == "url";
        });
    bool remove_proxy = proxy_url->baseline_present;
    if (!proxy_url->override->isChecked()) {
        for (const auto& field : fields_) {
            if (field.table != "proxy" || field.key == "url") {
                continue;
            }
            if ((field.override && field.override->isChecked()) ||
                (!field.override && field.kind != Kind::choice &&
                 !value(field).isEmpty()) ||
                (field.kind == Kind::choice && value(field) != "auto")) {
                remove_proxy = false;
            }
        }
        if (remove_proxy) {
            patches.push_back({"proxy", "", std::monostate{}});
        }
    }
    for (const auto& field : fields_) {
        if (field.table == "proxy" && remove_proxy &&
            !proxy_url->override->isChecked()) {
            continue;
        }
        const auto text = value(field);
        const bool present = !field.override || field.override->isChecked();
        if (text == field.baseline &&
            (!field.override || present == field.baseline_present)) {
            continue;
        }
        config::SettingValue setting = text.toStdString();
        if (field.kind == Kind::number) {
            bool valid = false;
            const auto number = text.toLongLong(&valid);
            const bool digits =
                !text.isEmpty() &&
                std::all_of(text.begin(), text.end(), [](QChar c) {
                    return c >= QChar{'0'} && c <= QChar{'9'};
                });
            if (!valid || !digits || number < 0) {
                throw std::invalid_argument(
                    field.table + "." + field.key +
                    ": expected an integer between 0 and 9223372036854775807"
                );
            }
            setting = static_cast<std::int64_t>(number);
        } else if (field.kind == Kind::boolean) {
            setting = text == "true";
        } else if (!present || ((field.kind == Kind::optional_path ||
                                 field.kind == Kind::optional_text) &&
                                text.isEmpty())) {
            setting = std::monostate{};
        } else if (field.kind == Kind::list ||
                   field.kind == Kind::override_list) {
            std::vector<std::string> entries;
            if (!text.isEmpty()) {
                for (const auto& entry : text.split('\n')) {
                    entries.push_back(entry.toStdString());
                }
            }
            setting = std::move(entries);
        }
        if (field.table == "sync" && field.key == "mode") {
            patches.push_back({"sync", "upload", std::monostate{}});
            // Keep the selected deletion policy when upload-only's implicit
            // default changes.
            const auto policy =
                std::find_if(fields_.begin(), fields_.end(), [](const auto& f) {
                    return f.key == "delete_policy";
                });
            patches.push_back(
                {"sync", "delete_policy", value(*policy).toStdString()}
            );
        }
        if (field.table == "sync" && field.key == "drive_ids") {
            patches.push_back({"sync", "drive_id", std::monostate{}});
        }
        patches.push_back({field.table, field.key, std::move(setting)});
    }
    return patches;
}

} // namespace onedrive::gui
