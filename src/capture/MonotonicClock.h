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

// End-to-end lag must be a single subtraction of two monotonic timestamps (never a sum of trace
// sub-spans). Start is the frame capturedAtNs (V4L2 monotonic buffer timestamp when available).
inline double monotonicLagMs(const qint64 startNs, const qint64 endNs)
{
    return static_cast<double>(endNs - startNs) / 1'000'000.0;
}

} // namespace consolation::capture
