#pragma once

#include <QMetaType>
#include <QString>
#include <vector>

namespace consolation::capture {

enum class CaptureBackend {
    Mock,
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

struct VideoTelemetrySnapshot {
    int width = 0;
    int height = 0;
    double configuredFps = 0.0;
    QString pixelFormat;
    double decodedFps = 0.0;
    double decodeAvgMs = 0.0;
    double decodeMaxMs = 0.0;
    double payloadAvgKb = 0.0;
    int bufferCount = 0;
};

} // namespace consolation::capture

Q_DECLARE_METATYPE(consolation::capture::VideoTelemetrySnapshot)
