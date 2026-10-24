#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "localization.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace onedrive::gui {

using window_detail::make_value_label;

void MainWindow::create_overview_page() {
    auto* overview = new QWidget{tabs_};
    overview->setObjectName("overviewPage");
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
    account_action_button_ = new QPushButton{tr("Log out"), overview};
    account_action_button_->setObjectName("accountActionButton");
    login_actions->addWidget(login_button_);
    login_actions->addWidget(account_action_button_);
    login_actions->addStretch();
    overview_layout->addRow(login_actions);
    startup_status_ = make_value_label({});
    startup_status_->setObjectName("startupStatus");
    overview_layout->addRow(startup_status_);
    overview_sync_status_ = make_value_label({});
    overview_sync_status_->setObjectName("overviewSyncStatus");
    overview_layout->addRow(tr("Synchronization"), overview_sync_status_);
    drive_type_value_ = make_value_label({});
    drive_type_value_->setObjectName("driveType");
    overview_layout->addRow(tr("Drive type"), drive_type_value_);
    tabs_->addTab(overview, tr("Overview"));
}

void MainWindow::create_sync_page() {
    sync_page_ = new QWidget{tabs_};
    sync_page_->setObjectName("syncPage");
    auto* sync_page_layout = new QVBoxLayout{sync_page_};
    sync_group_ = new QGroupBox{sync_page_};
    sync_group_->setObjectName("synchronizationGroup");
    auto* sync_layout = new QVBoxLayout{sync_group_};
    auto* sync_actions = new QHBoxLayout;
    preview_button_ = new QPushButton{sync_group_};
    preview_button_->setObjectName("previewSyncButton");
    sync_button_ = new QPushButton{sync_group_};
    sync_button_->setObjectName("startSyncButton");
    monitor_button_ = new QPushButton{sync_group_};
    monitor_button_->setObjectName("startMonitorButton");
    stop_sync_button_ = new QPushButton{sync_group_};
    stop_sync_button_->setObjectName("stopSyncButton");
    sync_actions->addWidget(preview_button_);
    sync_actions->addWidget(sync_button_);
    sync_actions->addWidget(monitor_button_);
    sync_actions->addWidget(stop_sync_button_);
    sync_actions->addStretch();
    sync_layout->addLayout(sync_actions);
    sync_status_ = make_value_label({});
    sync_status_->setObjectName("syncStatus");
    sync_layout->addWidget(sync_status_);
    monitor_status_ = make_value_label({});
    monitor_status_->setObjectName("monitorStatus");
    sync_layout->addWidget(monitor_status_);
    sync_context_ = make_value_label({});
    sync_context_->setObjectName("syncContext");
    sync_layout->addWidget(sync_context_);
    sync_progress_ = new QProgressBar{sync_group_};
    sync_progress_->setObjectName("syncProgress");
    sync_progress_->setAlignment(Qt::AlignCenter);
    sync_progress_->setStyleSheet(
        "QProgressBar#syncProgress::chunk { background-color: #0078d4; }"
    );
    sync_layout->addWidget(sync_progress_);
    sync_progress_hint_ = make_value_label({});
    sync_progress_hint_->setObjectName("syncProgressHint");
    sync_layout->addWidget(sync_progress_hint_);
    transfer_status_ = make_value_label({});
    transfer_status_->setObjectName("transferStatus");
    sync_layout->addWidget(transfer_status_);
    upload_progress_ = new QProgressBar{sync_group_};
    upload_progress_->setObjectName("uploadProgress");
    upload_progress_->setAlignment(Qt::AlignCenter);
    upload_progress_->setStyleSheet(
        "QProgressBar#uploadProgress::chunk { background-color: #0078d4; }"
    );
    sync_layout->addWidget(upload_progress_);
    upload_status_ = make_value_label({});
    upload_status_->setObjectName("uploadStatus");
    sync_layout->addWidget(upload_status_);
    monitor_totals_status_ = make_value_label({});
    monitor_totals_status_->setObjectName("monitorTotals");
    sync_layout->addWidget(monitor_totals_status_);
    sync_summary_status_ = make_value_label({});
    sync_summary_status_->setObjectName("syncSummary");
    sync_layout->addWidget(sync_summary_status_);
    sync_page_layout->addWidget(sync_group_);
    blocked_group_ = new QGroupBox{sync_page_};
    blocked_group_->setObjectName("blockedItemsGroup");
    auto* blocked_layout = new QVBoxLayout{blocked_group_};
    auto* blocked_actions = new QHBoxLayout;
    blocked_filter_ = new QLineEdit{blocked_group_};
    blocked_filter_->setObjectName("blockedItemsFilter");
    blocked_reason_ = new QComboBox{blocked_group_};
    blocked_reason_->setObjectName("blockedItemsReason");
    blocked_refresh_ = new QPushButton{blocked_group_};
    blocked_refresh_->setObjectName("refreshBlockedItemsButton");
    blocked_actions->addWidget(blocked_filter_, 1);
    blocked_actions->addWidget(blocked_reason_);
    blocked_actions->addWidget(blocked_refresh_);
    blocked_layout->addLayout(blocked_actions);
    blocked_status_ = make_value_label({});
    blocked_status_->setObjectName("blockedItemsStatus");
    blocked_layout->addWidget(blocked_status_);
    blocked_table_ = new QTreeWidget{blocked_group_};
    blocked_table_->setObjectName("blockedItemsTable");
    blocked_table_->setColumnCount(2);
    blocked_table_->setRootIsDecorated(false);
    blocked_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    blocked_table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    blocked_table_->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents
    );
    blocked_layout->addWidget(blocked_table_, 1);
    blocked_details_ = new QPlainTextEdit{blocked_group_};
    blocked_details_->setObjectName("blockedItemsDetails");
    blocked_details_->setReadOnly(true);
    blocked_details_->setMaximumHeight(130);
    blocked_layout->addWidget(blocked_details_);
    sync_page_layout->addWidget(blocked_group_, 1);
    connect(
        blocked_filter_,
        &QLineEdit::textChanged,
        this,
        &MainWindow::render_blocked_items
    );
    connect(
        blocked_reason_,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::render_blocked_items
    );
    connect(
        blocked_table_,
        &QTreeWidget::itemSelectionChanged,
        this,
        &MainWindow::render_blocked_details
    );
    connect(
        blocked_refresh_,
        &QPushButton::clicked,
        this,
        &MainWindow::refresh_blocked_items
    );
    connect(
        &synchronization_,
        &SyncController::blocked_items_ready,
        this,
        [this](
            const std::vector<storage::BlockedItem>& items, const QString& error
        ) {
            blocked_items_ = items;
            blocked_error_ = error;
            const QSignalBlocker blocker{blocked_reason_};
            const auto selected = blocked_reason_->currentData().toString();
            blocked_reason_->clear();
            blocked_reason_->addItem(tr("All reasons"), QString{});
            for (const auto& item : items) {
                const auto code = QString::fromStdString(item.reason_code);
                if (blocked_reason_->findData(code) < 0) {
                    blocked_reason_->addItem(code, code);
                }
            }
            const auto index = blocked_reason_->findData(selected);
            blocked_reason_->setCurrentIndex(index < 0 ? 0 : index);
            render_blocked_items();
        }
    );
    tabs_->addTab(sync_page_, tr("Sync"));
}

void MainWindow::create_header_controls() {
    auto* header_controls = new QWidget{tabs_};
    header_controls->setObjectName("headerControls");
    auto* header_layout = new QHBoxLayout{header_controls};
    header_layout->setContentsMargins(8, 0, 0, 0);
    if (localization_) {
        language_label_ = new QLabel{tr("Language"), header_controls};
        language_selector_ = new QComboBox{header_controls};
        language_selector_->setObjectName("languageSelector");
        language_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        language_label_->setBuddy(language_selector_);
        for (const auto language :
             {Language::automatic,
              Language::english,
              Language::simplified_chinese}) {
            language_selector_->addItem(QString{}, static_cast<int>(language));
        }
        header_layout->addWidget(language_label_);
        header_layout->addWidget(language_selector_);
        connect(
            language_selector_,
            &QComboBox::currentIndexChanged,
            this,
            [this](int index) {
                localization_->set_language(
                    static_cast<Language>(
                        language_selector_->itemData(index).toInt()
                    )
                );
                retranslate_ui();
            }
        );
    }
    about_button_ = new QPushButton{tr("About"), header_controls};
    about_button_->setObjectName("aboutButton");
    header_layout->addWidget(about_button_);
    tabs_->setCornerWidget(header_controls, Qt::TopRightCorner);
    connect(
        about_button_, &QPushButton::clicked, this, &MainWindow::show_about
    );
}

} // namespace onedrive::gui
