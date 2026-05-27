#pragma once

#include <QIcon>
#include <QPixmap>

namespace consolation::ui {

[[nodiscard]] QIcon createAppIcon();
[[nodiscard]] QPixmap appIconPixmap(int size, int cornerRadius = -1);

} // namespace consolation::ui
