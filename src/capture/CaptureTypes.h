#pragma once

#include <QImage>
#include <QMetaType>
#include <QString>

#include <memory>
#include <vector>

namespace consolation::capture {

// Shared ownership of decoded frame pixels. Queued cross-thread delivery copies only this
// handle (refcount), not the underlying QImage buffer.
using FrameHandle = std::shared_ptr<const QImage>;

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

enum class VideoFrameMemory {
    Unknown = 0,
    Mmap,
    DmaBuf,
    Mixed,
};

enum class VideoDisplayPath {
    Unknown = 0,
    Gpu,
    Cpu,
    Mixed,
};

enum class MjpegDecodePath {
    None = 0,
    Vaapi,
    V4l2M2m,
    Libyuv,
    Qt,
};

struct VideoTelemetrySnapshot {
    int width = 0;
    int height = 0;
    double configuredFps = 0.0;
    quint32 pixelFormatFourcc = 0;
    double decodedFps = 0.0;
    double decodeAvgMs = 0.0;
    double payloadAvgKb = 0.0;
    int bufferCount = 0;
    VideoFrameMemory frameMemory = VideoFrameMemory::Unknown;
    int dmaFramesInWindow = 0;
    int cpuFramesInWindow = 0;
    bool dmaCapturePathEnabled = false;
    MjpegDecodePath mjpegDecodePath = MjpegDecodePath::None;
    bool mjpegHwDecodeActive = false;
};

inline QString mjpegDecodePathLabel(const MjpegDecodePath path)
{
    switch (path) {
    case MjpegDecodePath::Vaapi:
        return QStringLiteral("VA-API");
    case MjpegDecodePath::V4l2M2m:
        return QStringLiteral("V4L2");
    case MjpegDecodePath::Libyuv:
        return QStringLiteral("CPU");
    case MjpegDecodePath::Qt:
        return QStringLiteral("Qt");
    default:
        return QStringLiteral("—");
    }
}

inline bool mjpegDecodePathIsHardware(const MjpegDecodePath path)
{
    return path == MjpegDecodePath::Vaapi || path == MjpegDecodePath::V4l2M2m;
}

inline QString frameMemoryLabel(const VideoFrameMemory memory)
{
    switch (memory) {
    case VideoFrameMemory::Mmap:
        return QStringLiteral("MMAP");
    case VideoFrameMemory::DmaBuf:
        return QStringLiteral("DMA-BUF");
    case VideoFrameMemory::Mixed:
        return QStringLiteral("MIXED");
    default:
        return QStringLiteral("—");
    }
}

inline QString displayPathLabel(const VideoDisplayPath path)
{
    switch (path) {
    case VideoDisplayPath::Gpu:
        return QStringLiteral("GPU");
    case VideoDisplayPath::Cpu:
        return QStringLiteral("CPU");
    case VideoDisplayPath::Mixed:
        return QStringLiteral("MIXED");
    default:
        return QStringLiteral("—");
    }
}

} // namespace consolation::capture

Q_DECLARE_METATYPE(consolation::capture::FrameHandle)
Q_DECLARE_METATYPE(consolation::capture::VideoTelemetrySnapshot)
