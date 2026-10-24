#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "onedrive/app/queries.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include <QComboBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace onedrive::gui {

using window_detail::make_value_label;

void MainWindow::create_files_page() {
    files_page_ = new QWidget{tabs_};
    files_page_->setObjectName("filesPage");
    auto* files_layout = new QVBoxLayout{files_page_};
    files_hint_ = make_value_label({});
    files_layout->addWidget(files_hint_);
    files_filter_ = new QLineEdit{files_page_};
    files_filter_->setObjectName("fileHistoryFilter");
    files_layout->addWidget(files_filter_);
    files_status_ = make_value_label({});
    files_status_->setObjectName("fileHistoryStatus");
    files_layout->addWidget(files_status_);
    files_table_ = new QTreeWidget{files_page_};
    files_table_->setObjectName("fileHistoryTable");
    files_table_->setColumnCount(5);
    files_table_->setRootIsDecorated(false);
    files_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const int column : {0, 1, 4}) {
        files_table_->header()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents
        );
    }
    files_table_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    files_table_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    files_layout->addWidget(files_table_, 1);
    tabs_->addTab(files_page_, tr("Files"));
    connect(
        files_filter_,
        &QLineEdit::textChanged,
        this,
        &MainWindow::filter_file_history
    );
    connect(
        &synchronization_,
        &SyncController::file_operations_ready,
        this,
        [this](
            const std::vector<events::FileOperationEvent>& operations,
            std::size_t retained,
            std::size_t omitted,
            std::size_t limit
        ) {
            auto* scroll = files_table_->verticalScrollBar();
            const bool at_bottom = scroll->value() == scroll->maximum();
            files_table_->setUpdatesEnabled(false);
            for (const auto& operation : operations) {
                auto* row = new QTreeWidgetItem{files_table_};
                row->setData(
                    0,
                    Qt::UserRole,
                    static_cast<qint64>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            operation.completed_at.time_since_epoch()
                        )
                            .count()
                    )
                );
                row->setData(
                    1, Qt::UserRole, static_cast<int>(operation.action)
                );
                row->setData(1, Qt::UserRole + 1, operation.directory);
                row->setData(1, Qt::UserRole + 2, operation.recovered);
                row->setText(2, QString::fromStdString(operation.path));
                row->setText(
                    3, QString::fromStdString(operation.previous_path)
                );
                row->setText(4, QString::fromStdString(operation.drive_id));
                row->setToolTip(
                    2, QString::fromStdString(operation.local_path)
                );
                render_file_history_row(row);
            }
            while (static_cast<std::size_t>(files_table_->topLevelItemCount()) >
                   retained) {
                delete files_table_->takeTopLevelItem(0);
            }
            files_omitted_ = omitted;
            files_limit_ = limit;
            render_file_history_status();
            files_table_->setUpdatesEnabled(true);
            if (at_bottom && !operations.empty()) {
                files_table_->scrollToBottom();
            }
        }
    );
}

QString MainWindow::file_action_text(
    events::FileAction action, bool directory, bool recovered
) const {
    const auto name = [&]() -> QString {
        switch (action) {
        case events::FileAction::downloaded:
            return tr("Downloaded");
        case events::FileAction::uploaded:
            return tr("Uploaded");
        case events::FileAction::moved_locally:
            return tr("Moved locally");
        case events::FileAction::moved_remotely:
            return tr("Moved remotely");
        case events::FileAction::deleted_locally:
            return tr("Deleted locally");
        case events::FileAction::deleted_remotely:
            return tr("Deleted remotely");
        }
        throw std::logic_error{"unknown file operation action"};
    }();
    auto result = directory ? tr("%1 (folder)").arg(name) : name;
    return recovered ? tr("%1 (recovered)").arg(result) : result;
}

void MainWindow::render_file_history_row(QTreeWidgetItem* row) {
    row->setText(
        0,
        QDateTime::fromMSecsSinceEpoch(row->data(0, Qt::UserRole).toLongLong())
            .toString("yyyy-MM-dd HH:mm:ss.zzz")
    );
    row->setText(
        1,
        file_action_text(
            static_cast<events::FileAction>(row->data(1, Qt::UserRole).toInt()),
            row->data(1, Qt::UserRole + 1).toBool(),
            row->data(1, Qt::UserRole + 2).toBool()
        )
    );
    const auto query = files_filter_->text().trimmed();
    bool matches = query.isEmpty();
    for (int column = 0; column < files_table_->columnCount() && !matches;
         ++column) {
        matches = row->text(column).contains(query, Qt::CaseInsensitive);
    }
    row->setHidden(!matches);
}

void MainWindow::render_file_history_status() {
    files_status_->setText(tr("%1 completed operations retained; %2 older "
                              "operations omitted (limit %3).")
                               .arg(files_table_->topLevelItemCount())
                               .arg(files_omitted_)
                               .arg(files_limit_));
}

void MainWindow::filter_file_history() {
    files_table_->setUpdatesEnabled(false);
    for (int index = 0; index < files_table_->topLevelItemCount(); ++index) {
        render_file_history_row(files_table_->topLevelItem(index));
    }
    files_table_->setUpdatesEnabled(true);
}

void MainWindow::render_file_history() {
    filter_file_history();
    render_file_history_status();
}

void MainWindow::refresh_blocked_items() {
    if (closing_) {
        return;
    }
    synchronization_.refresh_blocked_items(state_.config_file);
}

void MainWindow::render_blocked_items() {
    const auto* current = blocked_table_->currentItem();
    const auto selected =
        current ? current->data(0, Qt::UserRole).toString() : QString{};
    const QSignalBlocker blocker{blocked_table_};
    blocked_table_->clear();
    const auto search = blocked_filter_->text();
    const auto reason = blocked_reason_->currentData().toString().toStdString();
    const auto reason_filter =
        reason.empty() ? std::nullopt : std::optional<std::string_view>{reason};
    const auto text_matches = [&search](const std::string& text) {
        return QString::fromStdString(text).contains(
            search, Qt::CaseInsensitive
        );
    };
    for (const auto& item : blocked_items_) {
        if (!app::matches_blocked_item(item, reason_filter, text_matches)) {
            continue;
        }
        const auto path = QString::fromStdString(item.remote_path);
        const auto code = QString::fromStdString(item.reason_code);
        const auto message = QString::fromStdString(item.reason_message);
        auto* row = new QTreeWidgetItem{blocked_table_, {path, code}};
        const auto id = QString::fromStdString(item.remote_id);
        row->setData(0, Qt::UserRole, id);
        row->setToolTip(0, path);
        row->setToolTip(1, message);
        if (id == selected) {
            blocked_table_->setCurrentItem(row);
        }
    }
    blocked_group_->setTitle(
        tr("Blocked items (%1)").arg(blocked_items_.size())
    );
    blocked_status_->setText(
        !blocked_error_.isEmpty()
            ? tr("Cannot load blocked items: %1").arg(blocked_error_)
        : blocked_items_.empty()
            ? tr("No saved blocked items.")
            : tr("%1 of %2 items shown. Discoveries from previews, sync and "
                 "monitoring are saved here.")
                  .arg(blocked_table_->topLevelItemCount())
                  .arg(blocked_items_.size())
    );
    render_blocked_details();
}

void MainWindow::render_blocked_details() {
    const auto* row = blocked_table_->currentItem();
    if (!row) {
        blocked_details_->clear();
        blocked_details_->setPlaceholderText(
            tr("Select an item to view and copy its details.")
        );
        return;
    }
    const auto id = row->data(0, Qt::UserRole).toString().toStdString();
    const auto item =
        std::ranges::find(blocked_items_, id, &storage::BlockedItem::remote_id);
    if (item == blocked_items_.end()) {
        return;
    }
    blocked_details_->setPlainText(
        tr("Path: %1\nReason: %2\n%3\nItem ID: %4\nRecorded discoveries: %5")
            .arg(
                QString::fromStdString(item->remote_path),
                QString::fromStdString(item->reason_code),
                QString::fromStdString(item->reason_message),
                QString::fromStdString(item->remote_id)
            )
            .arg(item->attempt_count)
    );
    blocked_details_->appendPlainText(
        tr("Policy: %1").arg(blocked_policy_text(*item))
    );
}

QString
MainWindow::blocked_policy_text(const storage::BlockedItem& item) const {
    const auto& traits = sync::blocked_item_strategy(item.reason_code).traits();
    switch (traits.control) {
    case sync::BlockedPolicyControl::local_conflict_configuration:
        return tr(
            "Uses the configured local-conflict policy; changes are applied "
            "only after validation."
        );
    case sync::BlockedPolicyControl::remote_deletion_configuration:
        return tr(
            "Uses the configured remote-deletion safeguard; it is never "
            "bypassed automatically."
        );
    case sync::BlockedPolicyControl::fixed:
        break;
    }
    switch (traits.kind) {
    case sync::BlockedPolicyKind::unsupported_format:
        return tr(
            "Fixed policy: skip unsupported content and protect local copies. "
            "This policy cannot be changed."
        );
    case sync::BlockedPolicyKind::safety:
        return tr("Fixed safety policy: unsafe operations cannot be forced.");
    case sync::BlockedPolicyKind::resource:
        return tr(
            "Retry after resources recover; clear only after verified "
            "resolution."
        );
    case sync::BlockedPolicyKind::conflict:
    case sync::BlockedPolicyKind::remote_deletion:
        return tr(
            "Revalidate the conflict before retrying; unsafe operations cannot "
            "be forced."
        );
    case sync::BlockedPolicyKind::unknown:
        return tr(
            "Unrecognized reason: keep blocked, with no automatic retry or "
            "override."
        );
    }
    Q_UNREACHABLE();
}

} // namespace onedrive::gui
