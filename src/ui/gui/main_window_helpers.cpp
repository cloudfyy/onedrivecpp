#include "main_window_helpers.hpp"
#include "main_window.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/byte_size.hpp"
#include <QLabel>
#include <QLocale>

namespace onedrive::gui::window_detail {

QString short_detail(const QString& text) {
    const auto first_line = text.section('\n', 0, 0);
    return first_line.size() > 240 ? first_line.left(237) + "..." : first_line;
}

QString number(std::uint64_t value) {
    return QString::number(static_cast<qulonglong>(value));
}

QString drive_type_text(const std::string& type) {
    if (type == "personal") {
        return MainWindow::tr("OneDrive Personal");
    }
    if (type == "business") {
        return MainWindow::tr("OneDrive for Business");
    }
    if (type == "documentLibrary") {
        return MainWindow::tr("SharePoint document library");
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
