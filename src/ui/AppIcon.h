#pragma once

#include <QIcon>
#include <QPixmap>

namespace consolation::ui {

[[nodiscard]] QIcon createAppIcon();
[[nodiscard]] QPixmap appIconPixmap(int size);

} // namespace consolation::ui
