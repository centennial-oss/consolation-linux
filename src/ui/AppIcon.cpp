#include "ui/AppIcon.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

namespace consolation::ui {

QIcon createAppIcon()
{
    QPixmap pixmap(256, 256);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto bounds = QRectF(16, 16, 224, 224);
    painter.setBrush(QColor(34, 36, 42));
    painter.setPen(QPen(QColor(230, 232, 236), 8));
    painter.drawRoundedRect(bounds, 44, 44);

    const QPolygonF playTriangle({
        QPointF(102, 76),
        QPointF(102, 180),
        QPointF(178, 128),
    });
    painter.setBrush(QColor(84, 214, 151));
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(playTriangle);

    return QIcon(pixmap);
}

} // namespace consolation::ui
