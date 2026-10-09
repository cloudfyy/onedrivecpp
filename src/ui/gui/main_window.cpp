#include "main_window.hpp"

#include "configuration.hpp"

#include <QFileDialog>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>
#include <QUrl>

#include <exception>
#include <functional>
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

QWidget* directory_field(
    QLineEdit*& field,
    QWidget* parent,
    const std::function<void(QLineEdit*)>& browse
) {
    auto* container = new QWidget{parent};
    auto* layout = new QHBoxLayout{container};
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit{container};
    auto* button = new QPushButton{"Browse…", container};
    layout->addWidget(field);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, container, [field, browse] {
        browse(field);
    });
    return container;
}

} // namespace

MainWindow::MainWindow(
    AppStateViewModel state, AuthenticationFunction authenticate
)
    : state_{std::move(state)},
      login_{std::move(authenticate)} {
    setWindowTitle("OneDrive C++");
    resize(860, 520);

    auto* tabs = new QTabWidget{this};
    auto* overview = new QWidget{tabs};
    auto* overview_layout = new QFormLayout{overview};
    account_status_value_ = make_value_label({});
    config_file_value_ = make_value_label({});
    sync_directory_value_ = make_value_label({});
    state_directory_value_ = make_value_label({});
    overview_layout->addRow("Account", account_status_value_);
    overview_layout->addRow("Configuration file", config_file_value_);
    overview_layout->addRow("Sync directory", sync_directory_value_);
    overview_layout->addRow("State directory", state_directory_value_);
    login_status_ = make_value_label("Sign in to connect a OneDrive account.");
    login_status_->setObjectName("loginStatus");
    login_url_ = make_value_label({});
    overview_layout->addRow("Login", login_status_);
    overview_layout->addRow("Browser login", login_url_);
    auto* login_actions = new QHBoxLayout;
    login_button_ = new QPushButton{"Sign in", overview};
    login_button_->setObjectName("signInButton");
    cancel_login_button_ = new QPushButton{"Cancel login", overview};
    cancel_login_button_->setObjectName("cancelLoginButton");
    browser_button_ = new QPushButton{"Open browser", overview};
    browser_button_->setObjectName("openBrowserButton");
    cancel_login_button_->setEnabled(false);
    browser_button_->setEnabled(false);
    login_actions->addWidget(login_button_);
    login_actions->addWidget(browser_button_);
    login_actions->addWidget(cancel_login_button_);
    login_actions->addStretch();
    overview_layout->addRow(login_actions);
    tabs->addTab(overview, "Overview");

    auto* settings = new QWidget{tabs};
    settings_ = settings;
    settings_->setObjectName("settingsPage");
    auto* settings_layout = new QFormLayout{settings};
    auto* config_path = make_value_label({});
    config_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    settings_layout->addRow("Configuration file", config_path);
    settings_layout->addRow(
        "Sync directory",
        directory_field(
            sync_directory_edit_, settings, [this](QLineEdit* field) {
                browse_for_directory(field);
            }
        )
    );
    settings_layout->addRow(
        "State directory",
        directory_field(
            state_directory_edit_, settings, [this](QLineEdit* field) {
                browse_for_directory(field);
            }
        )
    );
    auto* theme = make_value_label("Follow system light/dark appearance");
    settings_layout->addRow("Appearance", theme);

    auto* actions = new QHBoxLayout;
    auto* import_button = new QPushButton{"Import TOML…", settings};
    auto* save_button = new QPushButton{"Save settings", settings};
    actions->addWidget(import_button);
    actions->addStretch();
    actions->addWidget(save_button);
    settings_layout->addRow(actions);
    tabs->addTab(settings, "Settings");

    setCentralWidget(tabs);
    config_path->setText(QString::fromStdString(state_.config_file.string()));
    connect(import_button, &QPushButton::clicked, this, [this] {
        import_settings();
    });
    connect(save_button, &QPushButton::clicked, this, [this] {
        save_settings();
    });
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
                "Complete authorization in your system browser."
            );
            authorization_url_ = url;
            login_status_->setText(
                "Waiting for browser authorization (up to 5 minutes)..."
            );
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
            authorization_url_.clear();
            browser_button_->setEnabled(false);
            cancel_login_button_->setEnabled(false);
            login_button_->setEnabled(true);
            settings_->setEnabled(true);
            if (closing_) {
                close();
                return;
            }
            if (cancelled) {
                login_status_->setText("Login cancelled.");
            } else if (!error.isEmpty()) {
                login_status_->setText("Login failed: " + error);
            } else {
                try {
                    state_ = load_app_state_view_model(state_.config_file);
                    account_status_value_->setText(
                        QString::fromStdString(state_.account_status)
                    );
                    login_status_->setText(
                        "Signed in. Account credentials saved."
                    );
                } catch (const std::exception& exception) {
                    login_status_->setText(
                        "Login completed, but account status could not be "
                        "refreshed: " +
                        QString::fromUtf8(exception.what())
                    );
                }
            }
        }
    );
    refresh_settings();
}

void MainWindow::start_login() {
    cancelling_ = false;
    settings_->setEnabled(false);
    login_button_->setEnabled(false);
    cancel_login_button_->setEnabled(true);
    login_status_->setText("Starting browser login...");
    try {
        login_.start(state_.config_file);
    } catch (const std::exception& error) {
        settings_->setEnabled(true);
        login_button_->setEnabled(true);
        cancel_login_button_->setEnabled(false);
        login_status_->setText(
            "Cannot start login: " + QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::cancel_login() {
    cancelling_ = true;
    login_url_->clear();
    authorization_url_.clear();
    browser_button_->setEnabled(false);
    cancel_login_button_->setEnabled(false);
    login_status_->setText("Cancelling login...");
    login_.cancel();
}

void MainWindow::open_login_url() {
    const QUrl url{authorization_url_, QUrl::StrictMode};
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty()) {
        login_status_->setText(
            "Cannot open browser: the login URL is not a valid HTTPS address."
        );
        login_.cancel();
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        login_status_->setText(
            "Cannot open the system browser. Copy the login URL below or try "
            "Open browser again."
        );
        login_url_->setText(authorization_url_);
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

void MainWindow::refresh_settings() {
    config_file_value_->setText(
        QString::fromStdString(state_.config_file.string())
    );
    account_status_value_->setText(
        QString::fromStdString(state_.account_status)
    );
    sync_directory_value_->setText(
        QString::fromStdString(state_.sync_directory.string())
    );
    state_directory_value_->setText(
        QString::fromStdString(state_.state_directory.string())
    );
    sync_directory_edit_->setText(
        QString::fromStdString(state_.sync_directory.string())
    );
    state_directory_edit_->setText(
        QString::fromStdString(state_.state_directory.string())
    );
}

void MainWindow::browse_for_directory(QLineEdit* field) {
    const auto selected = QFileDialog::getExistingDirectory(
        this, "Select directory", field->text()
    );
    if (!selected.isEmpty()) {
        field->setText(selected);
    }
}

void MainWindow::save_settings() {
    const auto new_sync_directory =
        std::filesystem::path{sync_directory_edit_->text().toStdString()};
    const auto new_state_directory =
        std::filesystem::path{state_directory_edit_->text().toStdString()};
    const bool sync_path_changed = new_sync_directory != state_.sync_directory;
    const bool state_path_changed =
        new_state_directory != state_.state_directory;
    if (sync_path_changed || state_path_changed) {
        auto summary = QString{
            "Save these directory changes?\n\n"
            "Existing files and synchronization state will not be moved."
        };
        if (sync_path_changed) {
            summary += "\n\nSync directory:\n" +
                       QString::fromStdString(state_.sync_directory.string()) +
                       "\n→ " +
                       QString::fromStdString(new_sync_directory.string()) +
                       "\nInspect a dry run before the next synchronization.";
        }
        if (state_path_changed) {
            summary += "\n\nState directory:\n" +
                       QString::fromStdString(state_.state_directory.string()) +
                       "\n→ " +
                       QString::fromStdString(new_state_directory.string()) +
                       "\nExisting account state and credentials will remain "
                       "in the old location.";
        }
        if (QMessageBox::question(
                this,
                "Confirm directory changes",
                summary,
                QMessageBox::Save | QMessageBox::Cancel
            ) != QMessageBox::Save) {
            return;
        }
    }

    try {
        const auto backup = save_basic_settings(
            state_.config_file, new_sync_directory, new_state_directory
        );
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        auto message = QString{"Basic settings saved."};
        if (backup) {
            message += "\nPrevious configuration backed up to:\n" +
                       QString::fromStdString(backup->string());
        }
        QMessageBox::information(this, "Settings saved", message);
    } catch (const std::exception& error) {
        QMessageBox::critical(
            this, "Cannot save settings", QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::import_settings() {
    const auto selected = QFileDialog::getOpenFileName(
        this, "Import configuration", {}, "TOML files (*.toml);;All files (*)"
    );
    if (selected.isEmpty()) {
        return;
    }

    const auto source = std::filesystem::path{selected.toStdString()};
    try {
        const auto preview = preview_configuration(source, state_.config_file);
        const auto message =
            QString{
                "Import this configuration to the active user "
                "configuration?\n\n"
                "Sync directory: %1\n"
                "State directory: %2\n"
                "Sync mode: %3\n"
                "Deletion policy: %4\n\n"
                "Relative file references will be resolved from the active "
                "configuration directory. The current configuration will be "
                "backed up."
            }
                .arg(QString::fromStdString(preview.sync_directory.string()))
                .arg(QString::fromStdString(preview.state_directory.string()))
                .arg(QString::fromStdString(preview.sync_mode))
                .arg(QString::fromStdString(preview.delete_policy));
        if (QMessageBox::question(
                this,
                "Import configuration",
                message,
                QMessageBox::Yes | QMessageBox::Cancel
            ) != QMessageBox::Yes) {
            return;
        }

        const auto backup = import_configuration(source, state_.config_file);
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        auto confirmation = QString{"Configuration imported."};
        if (backup) {
            confirmation += "\nPrevious configuration backed up to:\n" +
                            QString::fromStdString(backup->string());
        }
        QMessageBox::information(this, "Configuration imported", confirmation);
    } catch (const std::exception& error) {
        QMessageBox::critical(
            this, "Cannot import configuration", QString::fromUtf8(error.what())
        );
    }
}

} // namespace onedrive::gui
