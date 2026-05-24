#pragma once

#include <QString>
#include <vector>

namespace consolation::capture {

enum class CaptureBackend {
    Mock,
    PipeWire,
    V4L2,
};

struct CaptureFormat {
    int width = 0;
    int height = 0;
    double framesPerSecond = 0.0;
    QString pixelFormat;
    QString label;
};

struct CaptureDevice {
    CaptureBackend backend = CaptureBackend::Mock;
    QString devicePath;
    QString displayName;
    QString stableId;
    QString nodeName;
    QString v4l2DevicePath;
    quint32 backendNodeId = 0;
    std::vector<CaptureFormat> formats;
};

} // namespace consolation::capture
