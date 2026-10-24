#pragma once

#include <QString>
#include <cstdint>
#include <string>

class QLabel;
class QLocale;

namespace onedrive::gui::window_detail {

QString short_detail(const QString& text);
QString number(std::uint64_t value);
QString drive_type_text(const std::string& type);
QString sync_result_text(int result);
QString data_size(std::uint64_t bytes, const QLocale& locale);
QString duration(std::uint64_t seconds, const QLocale& locale);
QLabel* make_value_label(const QString& text);

} // namespace onedrive::gui::window_detail
