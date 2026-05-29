#pragma once

#include <cstdint>

namespace consolation::platform::linux::mjpeg_perf {

struct Sample {
    double parseMs = 0.0;
    double renderMs = 0.0;
    double exportMs = 0.0;
    double argbMs = 0.0;
    double totalMs = 0.0;
    bool dmaPath = false;
    bool reuseBuffers = false;
};

[[nodiscard]] bool loggingEnabled();

void recordVaapiSample(const Sample &sample);

void recordSessionDecode(const double totalMs, const bool dmaPath, const char *detail);

} // namespace consolation::platform::linux::mjpeg_perf

// Per-frame tracing, gated by the CONSOLATION_FRAME_TRACE environment variable. Unlike the
// accumulating mjpeg_perf logger above, these emit one stdout line per event so runs can be
// sorted by `seq` (a monotonic join key) to compare where the NV12 and MJPEG paths diverge.
namespace consolation::platform::linux::frame_trace {

[[nodiscard]] bool enabled();

// Monotonic per-frame sequence number. Allocated once per captured frame and carried through the
// decode and present spans so all four trace lines for one frame share a join key.
[[nodiscard]] uint64_t nextSeq();

// Capture thread, around VIDIOC_DQBUF (item 2).
struct CaptureSpan {
    uint64_t seq = 0;
    uint32_t fourcc = 0;
    int captureBufferIndex = -1;
    int64_t capturedAtNs = 0;
    // V4L2 capture timestamp to handleReadyRead entry (excludes drain/requeue/DQBUF work).
    double captureAgeMs = 0.0;
    // Same baseline at VIDIOC_DQBUF of the kept buffer (diagnostic dequeue overhead).
    double dqAgeMs = 0.0;
    int drainCount = 0;
    double dqToEmitMs = 0.0;
    int bytesUsed = 0;
    const char *path = "";
};
void recordCapture(const CaptureSpan &span);

// VA-API MJPEG decoder internals (item 3).
struct VaapiDecodeSpan {
    uint64_t seq = 0;
    bool dmaPath = false;
    double parseMs = 0.0;
    double sliceBufferMs = 0.0;
    double vaSubmitMs = 0.0;
    double vaSyncMs = 0.0;
    double exportFdMs = 0.0;
};
void recordVaapiDecode(const VaapiDecodeSpan &span);

// Renderer, per presented DMA frame (items 1 and 4).
struct PresentSpan {
    uint64_t seq = 0;
    int64_t capturedAtNs = 0;
    int captureBufferIndex = -1;
    // capturedAtNs → present time (one monotonic subtraction; never summed from sub-spans below).
    double lagMs = 0.0;
    const char *origin = "";
    double emitToPaintMs = 0.0;
    bool eglReused = false;
    double eglBindMs = 0.0;
    double drawMs = 0.0;
    double glFinishMs = 0.0;
};
void recordPresent(const PresentSpan &span);

// Buffer lifetime, at requeue / VA slot release (item 5).
struct BufferLifetimeSpan {
    uint64_t seq = 0;
    const char *event = "";
    int captureBufferIndex = -1;
    double v4l2HoldMs = 0.0;
    double requeueDelayMs = 0.0;
    int outstandingCaptureBuffers = 0;
    int vaSurfaceSlot = -1;
    double vaSlotHoldMs = 0.0;
    int outstandingVaSlots = 0;
};
void recordBufferLifetime(const BufferLifetimeSpan &span);

} // namespace consolation::platform::linux::frame_trace
