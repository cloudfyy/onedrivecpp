#pragma once

#include <QString>

namespace onedrive::gui {

[[nodiscard]] QString autostart_file_path();
[[nodiscard]] bool autostart_enabled();
void set_autostart_enabled(bool enabled);

} // namespace onedrive::gui
