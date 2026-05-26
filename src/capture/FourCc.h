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

} // namespace consolation::capture
