#include "settings_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <stdexcept>
#include <algorithm>

namespace onedrive::gui {

SettingsEditor::SettingsEditor(QWidget* parent)
    : QWidget{parent} {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget{this};
    tabs_->setObjectName("settingsTabs");
    layout->addWidget(tabs_);
    const auto page = [this] {
        auto* scroll = new QScrollArea{tabs_};
        scroll->setWidgetResizable(true);
        auto* contents = new QWidget{scroll};
        auto* form = new QFormLayout{contents};
        scroll->setWidget(contents);
        tabs_->addTab(scroll, {});
        return form;
    };
    const auto note = [this](QFormLayout* form, const char* text) {
        auto* label = new QLabel;
        label->setWordWrap(true);
        form->addRow(label);
        notes_.emplace_back(label, text);
    };
    const auto advanced = [this](QFormLayout* form, const char* title) {
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
    };
    auto* account = page();
    add(account,
        "auth",
        "application_id",
        QT_TR_NOOP("Application ID"),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.application_id); });
    add(account,
        "auth",
        "tenant_id",
        QT_TR_NOOP("Tenant ID"),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.azure_tenant_id);
        });
    add(
        account,
        "sync",
        "data_directory",
        QT_TR_NOOP("Sync directory"),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.sync_data_directory.string());
        },
        {},
        true
    );
    add(
        account,
        "state",
        "directory",
        QT_TR_NOOP("State directory"),
        Kind::text,
        [](const auto& c) {
            return QString::fromStdString(c.state_directory.string());
        },
        {},
        true
    );
    add(account,
        "sync",
        "drive_id",
        QT_TR_NOOP("Drive ID"),
        Kind::text,
        [](const auto& c) { return QString::fromStdString(c.drive_id); });
    note(
        account,
        QT_TR_NOOP(
            "Directory changes do not migrate files, account state or "
            "credentials. Settings take effect on the next operation; running "
            "operations are not changed."
        )
    );

    auto* sync = page();
    add(sync,
        "sync",
        "mode",
        QT_TR_NOOP("Sync mode"),
        Kind::choice,
        [](const auto& c) {
            switch (c.sync_mode) {
            case onedrive::sync::SyncMode::upload_only:
                return QString{"upload_only"};
            case onedrive::sync::SyncMode::download_only:
                return QString{"download_only"};
            default:
                return QString{"bidirectional"};
            }
        },
        {{"bidirectional", QT_TR_NOOP("Bidirectional")},
         {"upload_only", QT_TR_NOOP("Upload only")},
         {"download_only", QT_TR_NOOP("Download only")}});
    add(sync,
        "sync",
        "delete_policy",
        QT_TR_NOOP("Deletion policy"),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.delete_policy == onedrive::sync::DeletePolicy::preserve
                    ? "preserve"
                    : "propagate"
            };
        },
        {{"propagate", QT_TR_NOOP("Propagate deletions")},
         {"preserve", QT_TR_NOOP("Preserve files")}});
    add(sync,
        "sync",
        "local_conflict",
        QT_TR_NOOP("Local conflict policy"),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.local_conflict == config::LocalConflictPolicy::block
                    ? "block"
                    : "backup"
            };
        },
        {{"block", QT_TR_NOOP("Block")},
         {"backup", QT_TR_NOOP("Back up conflicts")}});
    add(sync,
        "sync",
        "sync_list",
        QT_TR_NOOP("Selective sync rules file (empty: disabled)"),
        Kind::optional_path,
        [](const auto& c) {
            return c.sync_list ? QString::fromStdString(c.sync_list->string())
                               : QString{};
        });
    add(sync,
        "sync",
        "dotfiles",
        QT_TR_NOOP("Dotfiles"),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.dotfiles == config::DotfilePolicy::include ? "include"
                                                             : "exclude"
            };
        },
        {{"include", QT_TR_NOOP("Include")},
         {"exclude", QT_TR_NOOP("Exclude")}});
    add(sync,
        "sync",
        "maximum_file_size_bytes",
        QT_TR_NOOP("Maximum file size (bytes; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_file_size_bytes);
        });
    auto* sync_advanced = advanced(sync, QT_TR_NOOP("Advanced sync rules"));
    add(sync_advanced,
        "sync",
        "nosync_enabled",
        QT_TR_NOOP("Honor .nosync files"),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.nosync_enabled ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "sync_root_files",
        QT_TR_NOOP("Include root files with selective sync"),
        Kind::boolean,
        [](const auto& c) {
            return QString{c.sync_root_files ? "true" : "false"};
        });
    add(sync_advanced,
        "sync",
        "maximum_remote_deletions",
        QT_TR_NOOP("Maximum remote deletions (0: no deletions)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.maximum_remote_deletions);
        });

    auto* transfer = page();
    add(transfer,
        "upload",
        "concurrency",
        QT_TR_NOOP("Upload concurrency (1–16)"),
        Kind::number,
        [](const auto& c) { return QString::number(c.upload_concurrency); });
    add(transfer,
        "download",
        "concurrency",
        QT_TR_NOOP("Download concurrency (1–16)"),
        Kind::number,
        [](const auto& c) { return QString::number(c.download_concurrency); });
    add(transfer,
        "upload",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Upload rate per request (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "download",
        "maximum_rate_bytes_per_second",
        QT_TR_NOOP("Download rate per request (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_rate_bytes_per_second);
        });
    add(transfer,
        "upload",
        "maximum_total_rate_bytes_per_second",
        QT_TR_NOOP("Aggregate upload rate (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.upload_maximum_total_rate_bytes_per_second
            );
        });
    add(transfer,
        "download",
        "maximum_total_rate_bytes_per_second",
        QT_TR_NOOP("Aggregate download rate (bytes/s; 0: unlimited)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(
                c.download_maximum_total_rate_bytes_per_second
            );
        });
    add(transfer,
        "transfer",
        "order",
        QT_TR_NOOP("Transfer order"),
        Kind::choice,
        [](const auto& c) {
            switch (c.transfer_order) {
            case config::TransferOrder::size_ascending:
                return QString{"size_asc"};
            case config::TransferOrder::size_descending:
                return QString{"size_dsc"};
            case config::TransferOrder::name_ascending:
                return QString{"name_asc"};
            case config::TransferOrder::name_descending:
                return QString{"name_dsc"};
            default:
                return QString{"default"};
            }
        },
        {{"default", QT_TR_NOOP("Default")},
         {"size_asc", QT_TR_NOOP("Smallest first")},
         {"size_dsc", QT_TR_NOOP("Largest first")},
         {"name_asc", QT_TR_NOOP("Name ascending")},
         {"name_dsc", QT_TR_NOOP("Name descending")}});
    auto* transfer_advanced =
        advanced(transfer, QT_TR_NOOP("Advanced transfer settings"));
    add(transfer_advanced,
        "upload",
        "chunk_size_bytes",
        QT_TR_NOOP(
            "Upload chunk (bytes; multiple of 327680, less than 62914560)"
        ),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.upload_chunk_size_bytes);
        });
    add(transfer_advanced,
        "download",
        "chunk_threshold_bytes",
        QT_TR_NOOP("Download chunk threshold (bytes; greater than 0)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_chunk_threshold_bytes);
        });
    add(transfer_advanced,
        "download",
        "checkpoint_interval_bytes",
        QT_TR_NOOP("Download checkpoint interval (bytes; greater than 0)"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_checkpoint_interval_bytes);
        });
    add(transfer_advanced,
        "download",
        "maximum_retries",
        QT_TR_NOOP("Maximum download retries"),
        Kind::number,
        [](const auto& c) {
            return QString::number(c.download_maximum_retries);
        });
    add(transfer_advanced,
        "download",
        "validation",
        QT_TR_NOOP("Download validation"),
        Kind::choice,
        [](const auto& c) {
            return QString{
                c.download_validation == config::DownloadValidationMode::strict
                    ? "strict"
                    : "relaxed"
            };
        },
        {{"strict", QT_TR_NOOP("Strict")}, {"relaxed", QT_TR_NOOP("Relaxed")}});
    retranslate();
}

void SettingsEditor::add(
    QFormLayout* form,
    const char* table,
    const char* key,
    const char* label,
    Kind kind,
    std::function<QString(const config::Config&)> read,
    std::vector<std::pair<const char*, const char*>> choices,
    bool directory
) {
    QWidget* widget;
    if (kind == Kind::choice) {
        auto* combo = new QComboBox;
        for (const auto& choice : choices) {
            combo->addItem(QString{}, QString::fromUtf8(choice.first));
        }
        widget = combo;
    } else if (kind == Kind::boolean) {
        widget = new QCheckBox;
    } else {
        auto* edit = new QLineEdit;
        if (kind == Kind::number) {
            edit->setPlaceholderText("0–9223372036854775807");
        }
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
            auto* edit = static_cast<QLineEdit*>(widget);
            const auto selected =
                directory ? QFileDialog::getExistingDirectory(
                                this, tr("Select directory"), edit->text()
                            )
                          : QFileDialog::getOpenFileName(
                                this, tr("Select rules file"), edit->text()
                            );
            if (!selected.isEmpty()) {
                edit->setText(selected);
            }
        });
    }
    form->addRow(label_widget, container);
    fields_.push_back(
        {table,
         key,
         label,
         kind,
         widget,
         label_widget,
         std::move(read),
         {},
         std::move(choices)}
    );
}

QString SettingsEditor::value(const Field& field) {
    if (field.kind == Kind::choice) {
        return static_cast<QComboBox*>(field.widget)->currentData().toString();
    }
    if (field.kind == Kind::boolean) {
        return static_cast<QCheckBox*>(field.widget)->isChecked() ? "true"
                                                                  : "false";
    }
    return static_cast<QLineEdit*>(field.widget)->text();
}

void SettingsEditor::load(const config::Config& config) {
    for (auto& field : fields_) {
        field.baseline = field.read(config);
        if (field.kind == Kind::choice) {
            auto* combo = static_cast<QComboBox*>(field.widget);
            combo->setCurrentIndex(combo->findData(field.baseline));
        } else if (field.kind == Kind::boolean) {
            static_cast<QCheckBox*>(field.widget)
                ->setChecked(field.baseline == "true");
        } else {
            static_cast<QLineEdit*>(field.widget)->setText(field.baseline);
        }
    }
}

void SettingsEditor::retranslate() {
    tabs_->setTabText(0, tr("Account and directories"));
    tabs_->setTabText(1, tr("Sync rules"));
    tabs_->setTabText(2, tr("Transfers"));
    for (auto& field : fields_) {
        field.label_widget->setText(tr(field.label));
        if (field.kind == Kind::choice) {
            auto* combo = static_cast<QComboBox*>(field.widget);
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
}

std::vector<config::SettingPatch> SettingsEditor::patches() const {
    std::vector<config::SettingPatch> patches;
    for (const auto& field : fields_) {
        const auto text = value(field);
        if (text == field.baseline) {
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
        } else if (field.kind == Kind::optional_path && text.isEmpty()) {
            setting = std::monostate{};
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
        patches.push_back({field.table, field.key, std::move(setting)});
    }
    return patches;
}

} // namespace onedrive::gui
