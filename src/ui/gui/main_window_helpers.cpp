#include "main_window_helpers.hpp"
#include "main_window.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/byte_size.hpp"
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QSize>
#include <utility>

namespace onedrive::gui {

namespace {

QIcon bundled_application_icon() {
    QIcon icon;
    for (const auto& [size, resource] : {
             std::pair{16, ":/icons/onedrive-cpp-16.png"},
             std::pair{24, ":/icons/onedrive-cpp-24.png"},
             std::pair{32, ":/icons/onedrive-cpp-32.png"},
             std::pair{48, ":/icons/onedrive-cpp-48.png"},
             std::pair{64, ":/icons/onedrive-cpp-64.png"},
             std::pair{128, ":/icons/onedrive-cpp-128.png"},
             std::pair{256, ":/icons/onedrive-cpp-256.png"},
             std::pair{512, ":/icons/onedrive-cpp-512.png"},
             std::pair{1024, ":/icons/onedrive-cpp-1024.png"},
         }) {
        icon.addFile(QString::fromLatin1(resource), QSize{size, size});
    }
    return icon;
}

} // namespace

QIcon application_icon() {
    QIcon bundled = bundled_application_icon();
    QIcon themed = QIcon::fromTheme("onedrive-cpp", bundled);
    if (themed.isNull() ||
        !themed.availableSizes().contains(QSize{1024, 1024})) {
        return bundled;
    }
    return themed;
}

} // namespace onedrive::gui

namespace onedrive::gui::window_detail {

QString short_detail(const QString& text) {
    const auto first_line = text.section('\n', 0, 0);
    return first_line.size() > 240 ? first_line.left(237) + "..." : first_line;
}

QString number(std::uint64_t value) {
    return QString::number(static_cast<qulonglong>(value));
}

QString drive_type_text(const std::string& type) {
    switch (account::parse_drive_kind(type)) {
    case account::DriveKind::personal:
        return MainWindow::tr("OneDrive Personal");
    case account::DriveKind::business:
        return MainWindow::tr("OneDrive for Business");
    case account::DriveKind::sharepoint_library:
        return MainWindow::tr("SharePoint document library");
    case account::DriveKind::unknown:
        return MainWindow::tr("Unknown drive type");
    }
    return MainWindow::tr("Unknown drive type");
}

QString sync_result_text(int result) {
    switch (result) {
    case exit_code::success:
        return MainWindow::tr("Succeeded");
    case exit_code::failure:
        return MainWindow::tr("Failed");
    case exit_code::blocked:
        return MainWindow::tr("Incomplete: blocked items");
    case exit_code::cancelled:
        return MainWindow::tr("Cancelled");
    default:
        return MainWindow::tr("Unknown result (exit code %1)").arg(result);
    }
}

QString data_size(std::uint64_t bytes, const QLocale& locale) {
    const auto size = util::scale_bytes(bytes);
    const auto amount = size.unit == "B"
                            ? locale.toString(static_cast<qulonglong>(bytes))
                            : locale.toString(size.value, 'f', 1);
    return QString{"%1 %2"}.arg(
        amount,
        QLatin1String{
            size.unit.data(), static_cast<qsizetype>(size.unit.size())
        }
    );
}

QString duration(std::uint64_t seconds, const QLocale& locale) {
    const auto amount = [&locale](std::uint64_t value) {
        return locale.toString(static_cast<qulonglong>(value));
    };
    if (seconds >= 86400) {
        return MainWindow::tr("%1 d %2 h")
            .arg(amount(seconds / 86400), amount(seconds % 86400 / 3600));
    }
    if (seconds >= 3600) {
        return MainWindow::tr("%1 h %2 min")
            .arg(amount(seconds / 3600), amount(seconds % 3600 / 60));
    }
    if (seconds >= 60) {
        return MainWindow::tr("%1 min %2 s")
            .arg(amount(seconds / 60), amount(seconds % 60));
    }
    return MainWindow::tr("%1 s").arg(amount(seconds));
}

QLabel* make_value_label(const QString& text) {
    auto* label = new QLabel{text};
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace onedrive::gui::window_detail
