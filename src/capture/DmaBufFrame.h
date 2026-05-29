#pragma once

#include <QMetaType>

#include <array>
#include <cstdint>
#include <memory>

namespace consolation::capture {

enum class DmaBufOrigin {
    V4L2Capture = 0,
    VaapiMjpeg,
};

enum class DmaBufLayout {
    Unknown = 0,
    Nv12,
    P010,
    Rgb888,
    Bgr888,
    Yuyv422,
    I420,
    Yv12,
};

// V4L2 capture buffer exported via VIDIOC_EXPBUF. The dma fd is owned by the capture session
// buffer pool; the handle only controls when the buffer is requeued (QBUF).
struct DmaBufFrame {
    static constexpr uint64_t invalidModifier = ~uint64_t { 0 };

    int bufferIndex = -1;
    int captureBufferIndex = -1;
    int dmaFd = -1;
    std::array<int, 2> planeFds { -1, -1 };
    std::array<int, 2> planeOffsets { 0, 0 };
    std::array<int, 2> planeStrides { 0, 0 };
    std::array<uint32_t, 2> planeFourccs { 0, 0 };
    std::array<uint64_t, 2> planeModifiers { invalidModifier, invalidModifier };
    int width = 0;
    int height = 0;
    int stride = 0;
    int bytesUsed = 0;
    DmaBufOrigin origin = DmaBufOrigin::V4L2Capture;
    DmaBufLayout layout = DmaBufLayout::Unknown;
    bool flipVertical = false;
    qint64 capturedAtNs = 0;
    // handleReadyRead entry time for this frame's capture wakeup (used for Work overlay stat).
    qint64 wakeAtNs = 0;
    // Frame-trace fields (only populated when CONSOLATION_FRAME_TRACE is set): seq is the join key
    // shared with the capture/decode traces; emittedAtNs marks when the capture thread published the
    // frame, so the renderer can measure emit-to-paint delivery latency.
    quint64 seq = 0;
    qint64 emittedAtNs = 0;
    // Always-populated monotonic id of this published dma frame (independent of frame-trace seq).
    // The UI thread uses it for "newest frame wins" coalescing: a delivered frame whose publishSeq
    // is older than CaptureSession::latestDmaSeq is dropped because a newer frame is already queued.
    quint64 publishSeq = 0;
};

using DmaBufFrameHandle = std::shared_ptr<DmaBufFrame>;

} // namespace consolation::capture

Q_DECLARE_METATYPE(consolation::capture::DmaBufFrameHandle)
