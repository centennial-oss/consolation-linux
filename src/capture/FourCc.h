#pragma once

#include <QString>

#include <QtGlobal>

namespace consolation::capture {

inline QString fourCcToString(const quint32 fourcc)
{
    const char bytes[4] = {
        static_cast<char>(fourcc & 0xFF),
        static_cast<char>((fourcc >> 8) & 0xFF),
        static_cast<char>((fourcc >> 16) & 0xFF),
        static_cast<char>((fourcc >> 24) & 0xFF),
    };
    return QString::fromLatin1(bytes, 4);
}

// Lower rank = more preferred for auto-select and format menus.
// NV12, YU12, I420-family, YUY*, RGB/BGR, P010, MJPEG.
inline int pixelFormatSelectionRank(const QString &pixelFormat)
{
    const auto upper = pixelFormat.toUpper();
    if (upper == QStringLiteral("NV12")) {
        return 0;
    }
    if (upper == QStringLiteral("YU12")) {
        return 1;
    }
    if (upper == QStringLiteral("I420") || upper == QStringLiteral("YV12") ||
        upper == QStringLiteral("YVU420")) {
        return 2;
    }
    if (upper == QStringLiteral("YUYV") || upper == QStringLiteral("YUY2")) {
        return 3;
    }
    if (upper == QStringLiteral("BGR3") || upper == QStringLiteral("RGB3") ||
        upper == QStringLiteral("BGR24") || upper == QStringLiteral("RGB24")) {
        return 4;
    }
    if (upper == QStringLiteral("P010")) {
        return 5;
    }
    if (upper == QStringLiteral("MJPG") || upper == QStringLiteral("MJPEG") ||
        upper == QStringLiteral("JPEG")) {
        return 6;
    }
    return 99;
}

inline QString pixelFormatDisplayName(const QString &pixelFormat)
{
    if (pixelFormat.isEmpty()) {
        return pixelFormat;
    }
    const auto upper = pixelFormat.toUpper();
    if (upper == QStringLiteral("MJPG") || upper == QStringLiteral("JPEG")) {
        return QStringLiteral("MJPEG");
    }
    return pixelFormat;
}

} // namespace consolation::capture
