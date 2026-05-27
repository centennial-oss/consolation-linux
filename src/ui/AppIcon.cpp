#include "ui/AppIcon.h"

#include <QFile>
#include <QPainter>
#include <QPainterPath>

namespace consolation::ui {

namespace {

constexpr auto appIconResource = ":/app/app-icon.png";

int defaultCornerRadiusForSize(const int size)
{
    return size >= 64 ? 14 : 10;
}

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

QPixmap appIconPixmap(const int size, int cornerRadius)
{
    const auto source = loadAppIconSource();
    if (source.isNull()) {
        return {};
    }

    if (cornerRadius < 0) {
        cornerRadius = defaultCornerRadiusForSize(size);
    }

    QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() != size || scaled.height() != size) {
        const auto x = (scaled.width() - size) / 2;
        const auto y = (scaled.height() - size) / 2;
        scaled = scaled.copy(x, y, size, size);
    }

    QPixmap rounded(size, size);
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clipPath;
    clipPath.addRoundedRect(QRectF(0, 0, size, size), cornerRadius, cornerRadius);
    painter.setClipPath(clipPath);
    painter.drawPixmap(0, 0, scaled);
    painter.end();

    return rounded;
}

QIcon createAppIcon()
{
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        const auto pixmap = appIconPixmap(size, defaultCornerRadiusForSize(size));
        if (!pixmap.isNull()) {
            icon.addPixmap(pixmap);
        }
    }
    return icon;
}

} // namespace consolation::ui
