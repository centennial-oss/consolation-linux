#include "ui/AppIcon.h"

#include <QFile>

namespace consolation::ui {

namespace {

constexpr auto appIconResource = ":/app/app-icon-large-rounded.png";

QPixmap loadAppIconSource()
{
    QPixmap source;
    if (QFile resourceFile(appIconResource); resourceFile.open(QIODevice::ReadOnly)) {
        source.loadFromData(resourceFile.readAll(), "PNG");
    }
    if (source.isNull()) {
        source = QPixmap(appIconResource);
    }
    return source;
}

} // namespace

QPixmap appIconPixmap(const int size)
{
    const auto source = loadAppIconSource();
    if (source.isNull()) {
        return {};
    }

    QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() != size || scaled.height() != size) {
        const auto x = (scaled.width() - size) / 2;
        const auto y = (scaled.height() - size) / 2;
        scaled = scaled.copy(x, y, size, size);
    }
    return scaled;
}

QIcon createAppIcon()
{
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        const auto pixmap = appIconPixmap(size);
        if (!pixmap.isNull()) {
            icon.addPixmap(pixmap);
        }
    }
    return icon;
}

} // namespace consolation::ui
