#include "main_window.hpp"

#include "configuration.hpp"
#include "localization.hpp"
#include "settings_editor.hpp"
#include "onedrive/version.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QFileDialog>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>
#include <QUrl>

#include <exception>
#include <utility>

namespace onedrive::gui {
namespace {

QLabel* make_value_label(const QString& text) {
    auto* label = new QLabel{text};
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace

MainWindow::MainWindow(
    AppStateViewModel state,
    AuthenticationFunction authenticate,
    Localization* localization
)
    : state_{std::move(state)},
      localization_{localization},
      login_{std::move(authenticate)} {
    setWindowTitle(tr("onedrive client"));
    resize(860, 520);

    auto* tabs = new QTabWidget{this};
    tabs_ = tabs;
    auto* overview = new QWidget{tabs};
    auto* overview_layout = new QFormLayout{overview};
    overview_layout_ = overview_layout;
    account_status_value_ = make_value_label({});
    config_file_value_ = make_value_label({});
    sync_directory_value_ = make_value_label({});
    state_directory_value_ = make_value_label({});
    overview_layout->addRow(tr("Account"), account_status_value_);
    overview_layout->addRow(tr("Configuration file"), config_file_value_);
    overview_layout->addRow(tr("Sync directory"), sync_directory_value_);
    overview_layout->addRow(tr("State directory"), state_directory_value_);
    login_status_ =
        make_value_label(tr("Sign in to connect a OneDrive account."));
    login_status_->setObjectName("loginStatus");
    login_url_ = make_value_label({});
    overview_layout->addRow(tr("Login"), login_status_);
    overview_layout->addRow(tr("Browser login"), login_url_);
    auto* login_actions = new QHBoxLayout;
    login_button_ = new QPushButton{tr("Sign in"), overview};
    login_button_->setObjectName("signInButton");
    cancel_login_button_ = new QPushButton{tr("Cancel login"), overview};
    cancel_login_button_->setObjectName("cancelLoginButton");
    browser_button_ = new QPushButton{tr("Open browser"), overview};
    browser_button_->setObjectName("openBrowserButton");
    cancel_login_button_->setEnabled(false);
    browser_button_->setEnabled(false);
    login_actions->addWidget(login_button_);
    login_actions->addWidget(browser_button_);
    login_actions->addWidget(cancel_login_button_);
    login_actions->addStretch();
    overview_layout->addRow(login_actions);
    startup_status_ = make_value_label({});
    startup_status_->setObjectName("startupStatus");
    overview_layout->addRow(startup_status_);
    tabs->addTab(overview, tr("Overview"));

    auto* settings = new QWidget{tabs};
    settings_ = settings;
    settings_->setObjectName("settingsPage");
    auto* settings_layout = new QFormLayout{settings};
    settings_layout_ = settings_layout;
    auto* config_path = make_value_label({});
    config_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    settings_layout->addRow(tr("Configuration file"), config_path);
    settings_editor_ = new SettingsEditor{settings};
    settings_layout->addRow(settings_editor_);
    sync_directory_edit_ =
        settings_editor_->findChild<QLineEdit*>("sync.data_directory");
    state_directory_edit_ =
        settings_editor_->findChild<QLineEdit*>("state.directory");
    auto* theme = make_value_label(tr("Follow system light/dark appearance"));
    theme_value_ = theme;
    settings_layout->addRow(tr("Appearance"), theme);

    auto* actions = new QHBoxLayout;
    auto* import_button = new QPushButton{tr("Import TOML…"), settings};
    auto* save_button = new QPushButton{tr("Save settings"), settings};
    import_button_ = import_button;
    save_button_ = save_button;
    import_button_->setObjectName("importSettingsButton");
    save_button_->setObjectName("saveSettingsButton");
    reload_button_ = new QPushButton{tr("Reload configuration"), settings};
    reload_button_->setObjectName("reloadSettingsButton");
    actions->addWidget(import_button);
    actions->addWidget(reload_button_);
    actions->addStretch();
    actions->addWidget(save_button);
    settings_layout->addRow(actions);
    settings_status_ = make_value_label({});
    settings_status_->setObjectName("settingsStatus");
    settings_layout->addRow(settings_status_);
    tabs->addTab(settings, tr("Settings"));

    setCentralWidget(tabs);
    if (localization_) {
        language_menu_ = menuBar()->addMenu(tr("Language"));
        language_menu_->setObjectName("languageMenu");
        language_actions_ = new QActionGroup{this};
        language_actions_->setExclusive(true);
        for (const auto language :
             {Language::automatic,
              Language::english,
              Language::simplified_chinese}) {
            auto* action = language_menu_->addAction(QString{});
            action->setCheckable(true);
            action->setData(static_cast<int>(language));
            language_actions_->addAction(action);
            connect(action, &QAction::triggered, this, [this, language] {
                localization_->set_language(language);
                retranslate_ui();
            });
        }
    }
    help_menu_ = menuBar()->addMenu(tr("Help"));
    help_menu_->setObjectName("helpMenu");
    about_action_ = help_menu_->addAction(tr("About"));
    about_action_->setObjectName("aboutAction");
    connect(about_action_, &QAction::triggered, this, &MainWindow::show_about);
    config_path->setText(QString::fromStdString(state_.config_file.string()));
    connect(import_button, &QPushButton::clicked, this, [this] {
        import_settings();
    });
    connect(save_button, &QPushButton::clicked, this, [this] {
        save_settings();
    });
    connect(reload_button_, &QPushButton::clicked,
            this, &MainWindow::reload_settings);
    connect(
        login_button_, &QPushButton::clicked, this, &MainWindow::start_login
    );
    connect(
        cancel_login_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::cancel_login
    );
    connect(
        browser_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::open_login_url
    );
    connect(
        &login_,
        &LoginController::browser_requested,
        this,
        [this](const QString& url) {
            if (cancelling_ || closing_) {
                return;
            }
            login_url_->setText(
                tr("Complete authorization in your system browser.")
            );
            login_url_is_instruction_ = true;
            authorization_url_ = url;
            set_login_status(QT_TR_NOOP(
                "Waiting for browser authorization (up to 5 minutes)..."
            ));
            browser_button_->setEnabled(true);
            open_login_url();
        },
        Qt::QueuedConnection
    );
    connect(
        &login_,
        &LoginController::finished,
        this,
        [this](const QString& error, bool cancelled) {
            login_url_->clear();
            login_url_is_instruction_ = false;
            authorization_url_.clear();
            browser_button_->setEnabled(false);
            cancel_login_button_->setEnabled(false);
            login_button_->setEnabled(state_.can_sign_in());
            settings_->setEnabled(true);
            if (closing_) {
                close();
                return;
            }
            if (cancelled) {
                set_login_status(QT_TR_NOOP("Login cancelled."));
            } else if (!error.isEmpty()) {
                set_login_status(QT_TR_NOOP("Login failed: %1"), error);
            } else {
                try {
                    state_ = load_app_state_view_model(state_.config_file);
                    refresh_settings(true);
                    if (state_.can_sign_in()) {
                        set_login_status(
                            QT_TR_NOOP("Signed in. Account credentials saved.")
                        );
                    } else {
                        const auto& detail =
                            !state_.configuration_error.empty()
                                ? state_.configuration_error
                            : !state_.account_error.empty()
                                ? state_.account_error
                                : state_.authentication_error;
                        set_login_status(
                            QT_TR_NOOP(
                                "Login completed, but account status could not be "
                                "refreshed: %1"
                            ),
                            QString::fromStdString(detail)
                        );
                    }
                } catch (const std::exception& exception) {
                    set_login_status(
                        QT_TR_NOOP(
                            "Login completed, but account status could not be "
                            "refreshed: %1"
                        ),
                        QString::fromUtf8(exception.what())
                    );
                }
            }
        }
    );
    refresh_settings();
    retranslate_ui();
    if (state_.configuration_status != ConfigurationStatus::valid) {
        tabs_->setCurrentWidget(settings_);
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && tabs_) {
        retranslate_ui();
    }
}

void MainWindow::retranslate_ui() {
    setWindowTitle(tr("onedrive client"));
    help_menu_->setTitle(tr("Help"));
    about_action_->setText(tr("About"));
    QApplication::setApplicationDisplayName(windowTitle());
    tabs_->setTabText(0, tr("Overview"));
    tabs_->setTabText(1, tr("Settings"));
    const auto set_label =
        [](QFormLayout* layout, int row, const QString& text) {
            auto* label = qobject_cast<QLabel*>(
                layout->itemAt(row, QFormLayout::LabelRole)->widget()
            );
            label->setText(text);
        };
    set_label(overview_layout_, 0, tr("Account"));
    set_label(overview_layout_, 1, tr("Configuration file"));
    set_label(overview_layout_, 2, tr("Sync directory"));
    set_label(overview_layout_, 3, tr("State directory"));
    set_label(overview_layout_, 4, tr("Login"));
    set_label(overview_layout_, 5, tr("Browser login"));
    set_label(settings_layout_, 0, tr("Configuration file"));
    set_label(settings_layout_, 2, tr("Appearance"));
    settings_editor_->retranslate();
    theme_value_->setText(tr("Follow system light/dark appearance"));
    login_button_->setText(tr("Sign in"));
    cancel_login_button_->setText(tr("Cancel login"));
    browser_button_->setText(tr("Open browser"));
    import_button_->setText(tr("Import TOML…"));
    save_button_->setText(tr("Save settings"));
    reload_button_->setText(tr("Reload configuration"));
    for (auto* browse : settings_->findChildren<QPushButton*>("browseButton")) {
        browse->setText(tr("Browse…"));
    }
    account_status_value_->setText(account_status_text(state_.account_status));
    refresh_startup_status();
    set_login_status(login_status_source_, login_status_detail_);
    if (login_url_is_instruction_) {
        login_url_->setText(
            tr("Complete authorization in your system browser.")
        );
    }
    if (language_menu_) {
        language_menu_->setTitle(tr("Language"));
        for (auto* action : language_actions_->actions()) {
            const auto language = static_cast<Language>(action->data().toInt());
            switch (language) {
            case Language::automatic:
                action->setText(tr("System language"));
                break;
            case Language::english:
                action->setText(tr("English"));
                break;
            case Language::simplified_chinese:
                action->setText(tr("Simplified Chinese"));
                break;
            }
            action->setChecked(localization_->language() == language);
        }
    }
}

void MainWindow::show_about() {
    QMessageBox dialog{this};
    dialog.setObjectName("aboutDialog");
    dialog.setWindowTitle(tr("About OneDrive client"));
    dialog.setIcon(QMessageBox::Information);
    dialog.setTextFormat(Qt::RichText);
    dialog.setText(
        tr("<b>OneDrive client</b><br>"
           "Version: %1<br>"
           "An independent, open-source OneDrive client.<br>"
           "License: GPL-3.0-or-later<br>"
           "Project: <a href=\"https://github.com/cloudfyy/onedrivecpp\">"
           "https://github.com/cloudfyy/onedrivecpp</a>")
            .arg(QString::fromLatin1(build_info::version))
    );
    dialog.setStandardButtons(QMessageBox::Ok);
    dialog.exec();
}

void MainWindow::set_login_status(const char* source, const QString& detail) {
    login_status_source_ = source;
    login_status_detail_ = detail;
    auto text = tr(source);
    if (!detail.isNull()) {
        text = text.arg(detail);
    }
    login_status_->setText(text);
}

void MainWindow::start_login() {
    if (!state_.can_sign_in()) {
        return;
    }
    cancelling_ = false;
    settings_->setEnabled(false);
    login_button_->setEnabled(false);
    cancel_login_button_->setEnabled(true);
    set_login_status(QT_TR_NOOP("Starting browser login..."));
    try {
        login_.start(state_.config_file);
    } catch (const std::exception& error) {
        settings_->setEnabled(true);
        login_button_->setEnabled(state_.can_sign_in());
        cancel_login_button_->setEnabled(false);
        set_login_status(
            QT_TR_NOOP("Cannot start login: %1"),
            QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::cancel_login() {
    cancelling_ = true;
    login_url_->clear();
    login_url_is_instruction_ = false;
    authorization_url_.clear();
    browser_button_->setEnabled(false);
    cancel_login_button_->setEnabled(false);
    set_login_status(QT_TR_NOOP("Cancelling login..."));
    login_.cancel();
}

void MainWindow::open_login_url() {
    const QUrl url{authorization_url_, QUrl::StrictMode};
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty()) {
        set_login_status(QT_TR_NOOP(
            "Cannot open browser: the login URL is not a valid HTTPS address."
        ));
        login_.cancel();
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        set_login_status(QT_TR_NOOP(
            "Cannot open the system browser. Copy the login URL below or try "
            "Open browser again."
        ));
        login_url_->setText(authorization_url_);
        login_url_is_instruction_ = false;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (login_.running()) {
        closing_ = true;
        cancel_login();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::refresh_settings(bool preserve_edits) {
    config_file_value_->setText(
        QString::fromStdString(state_.config_file.string())
    );
    account_status_value_->setText(account_status_text(state_.account_status));
    sync_directory_value_->setText(
        QString::fromStdString(state_.sync_directory.string())
    );
    state_directory_value_->setText(
        QString::fromStdString(state_.state_directory.string())
    );
    if (!preserve_edits) {
        if (state_.configuration_status != ConfigurationStatus::invalid) {
            bool missing = false;
            settings_editor_->load(
                load_configuration(state_.config_file, missing)
            );
        }
    }
    const bool editable =
        state_.configuration_status != ConfigurationStatus::invalid;
    sync_directory_edit_->setEnabled(editable);
    state_directory_edit_->setEnabled(editable);
    settings_editor_->setEnabled(editable);
    save_button_->setEnabled(editable);
    login_button_->setEnabled(state_.can_sign_in());
    refresh_startup_status();
    if (!state_.can_sign_in()) {
        set_login_status(QT_TR_NOOP(
            "Sign in is unavailable. Resolve the errors in Settings, then "
            "reload the configuration."
        ));
    } else if (state_.account_status == AccountStatus::credentials_available) {
        set_login_status(QT_TR_NOOP(
            "Saved credentials have not been verified online. "
            "No sign-in is required at startup."
        ));
    } else {
        set_login_status(QT_TR_NOOP("Sign in to connect a OneDrive account."));
    }
}

void MainWindow::refresh_startup_status() {
    QStringList messages;
    if (state_.configuration_status == ConfigurationStatus::missing) {
        messages << tr(
            "Configuration file is missing. Using defaults; complete setup "
            "in Settings."
        );
    } else if (state_.configuration_status == ConfigurationStatus::invalid) {
        messages << tr(
            "Configuration could not be loaded:\n%1\n"
            "Import a valid TOML file, or repair this file and reload. "
            "The original file has not been changed."
        ).arg(QString::fromStdString(state_.configuration_error));
    }
    if (!state_.authentication_error.empty()) {
        messages << tr("Authentication configuration needs attention:\n%1")
                        .arg(QString::fromStdString(state_.authentication_error));
    }
    if (!state_.account_error.empty()) {
        messages << tr(
            "Local account credentials could not be read:\n%1\n"
            "Repair the state directory or select another one in Settings, "
            "then reload."
        ).arg(QString::fromStdString(state_.account_error));
    }
    if (!state_.sync_error.empty()) {
        messages << tr(
            "Sync directory needs attention before synchronization:\n%1"
        ).arg(QString::fromStdString(state_.sync_error));
    }
    startup_status_->setText(messages.join("\n\n"));
    settings_status_->setText(startup_status_->text());
}

void MainWindow::reload_settings() {
    state_ = load_app_state_view_model(state_.config_file);
    refresh_settings();
}

void MainWindow::save_settings() {
    std::vector<config::SettingPatch> patches;
    try {
        patches = settings_editor_->patches();
    } catch (const std::exception& error) {
        QMessageBox::critical(
            this,
            tr("Cannot save settings"),
            tr("Settings could not be saved:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
        return;
    }
    const auto new_sync_directory =
        std::filesystem::path{sync_directory_edit_->text().toStdString()};
    const auto new_state_directory =
        std::filesystem::path{state_directory_edit_->text().toStdString()};
    const bool sync_path_changed = new_sync_directory != state_.sync_directory;
    const bool state_path_changed =
        new_state_directory != state_.state_directory;
    if (sync_path_changed || state_path_changed) {
        auto summary =
            tr("Save these directory changes?\n\n"
               "Existing files and synchronization state will not be moved.");
        if (sync_path_changed) {
            summary +=
                tr("\n\nSync directory:\n%1\n→ %2\n"
                   "Inspect a dry run before the next synchronization.")
                    .arg(
                        QString::fromStdString(state_.sync_directory.string()),
                        QString::fromStdString(new_sync_directory.string())
                    );
        }
        if (state_path_changed) {
            summary +=
                tr("\n\nState directory:\n%1\n→ %2\n"
                   "Existing account state and credentials will remain "
                   "in the old location.")
                    .arg(
                        QString::fromStdString(state_.state_directory.string()),
                        QString::fromStdString(new_state_directory.string())
                    );
        }
        if (QMessageBox::question(
                this,
                tr("Confirm directory changes"),
                summary,
                QMessageBox::Save | QMessageBox::Cancel
            ) != QMessageBox::Save) {
            return;
        }
    }

    try {
        const auto backup = save_settings_patch(state_.config_file, patches);
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        auto message =
            tr("Settings saved. Changes take effect on the next operation.");
        if (backup) {
            message += tr("\nPrevious configuration backed up to:\n%1")
                           .arg(QString::fromStdString(backup->string()));
        }
        QMessageBox::information(this, tr("Settings saved"), message);
    } catch (const std::exception& error) {
        QMessageBox::critical(
            this,
            tr("Cannot save settings"),
            tr("Settings could not be saved:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
    }
}

void MainWindow::import_settings() {
    const auto selected = QFileDialog::getOpenFileName(
        this,
        tr("Import configuration"),
        {},
        tr("TOML files (*.toml);;All files (*)")
    );
    if (selected.isEmpty()) {
        return;
    }

    const auto source = std::filesystem::path{selected.toStdString()};
    try {
        const auto preview = preview_configuration(source, state_.config_file);
        const auto message =
            tr("Import this configuration to the active user "
               "configuration?\n\n"
               "Sync directory: %1\n"
               "State directory: %2\n"
               "Sync mode: %3\n"
               "Deletion policy: %4\n\n"
               "Relative file references will be resolved from the active "
               "configuration directory. The current configuration will be "
               "backed up.")
                .arg(
                    QString::fromStdString(preview.sync_directory.string()),
                    QString::fromStdString(preview.state_directory.string()),
                    sync_mode_text(preview.sync_mode),
                    delete_policy_text(preview.delete_policy)
                );
        if (QMessageBox::question(
                this,
                tr("Import configuration"),
                message,
                QMessageBox::Yes | QMessageBox::Cancel
            ) != QMessageBox::Yes) {
            return;
        }

        const auto backup = import_configuration(source, state_.config_file);
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        auto confirmation = tr("Configuration imported.");
        if (backup) {
            confirmation += tr("\nPrevious configuration backed up to:\n%1")
                                .arg(QString::fromStdString(backup->string()));
        }
        QMessageBox::information(
            this, tr("Configuration imported"), confirmation
        );
    } catch (const std::exception& error) {
        QMessageBox::critical(
            this,
            tr("Cannot import configuration"),
            tr("Configuration could not be imported:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
    }
}

} // namespace onedrive::gui
