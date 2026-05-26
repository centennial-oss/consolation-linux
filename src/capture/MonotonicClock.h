#pragma once

#include <QtGlobal>

#include <chrono>

namespace consolation::capture {

inline qint64 monotonicClockNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace consolation::capture
