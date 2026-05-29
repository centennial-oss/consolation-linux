#pragma once

#include "capture/CaptureSession.h"
#include "capture/FrameBufferPool.h"
#include "platform/linux/MjpegDecoder.h"
#include "platform/linux/MjpegDecoderBackends.h"

#include <QSocketNotifier>

#include <linux/videodev2.h>

#include <array>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace consolation::platform::linux {

class V4L2CaptureSession final : public capture::CaptureSession {
    Q_OBJECT

public:
    // Driver mmap slots (not display queue depth). Extra slots add ~buffer.length RAM each; the
    // driver may allocate fewer than requested. Helps DMA path when the UI briefly holds a buffer.
    static constexpr unsigned captureBufferCount = 8;

    explicit V4L2CaptureSession(QObject *parent = nullptr);
    ~V4L2CaptureSession() override;

    [[nodiscard]] bool start(const capture::CaptureDevice &device, const capture::CaptureFormat &format) override;
    void stop() override;

    void setDmaBufDisplayRequested(const bool requested);

    Q_INVOKABLE void finishDmaFrameAsCpu(capture::DmaBufFrameHandle frame);
    Q_INVOKABLE void recordDmaFramePresented(int bytesUsed);
    // Decode-side telemetry, posted from the MJPEG decode worker so it is applied on the capture thread.
    Q_INVOKABLE void recordVaapiDmaDecodeTelemetry(double decodeMs);

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
        int dmaFd = -1;
        qint64 capturedAtNs = 0;
        qint64 presentationBaseNs = 0;
        qint64 wakeAtNs = 0;
        // Frame-trace bookkeeping (CONSOLATION_FRAME_TRACE): monotonic DQBUF time and the seq of the
        // last frame handed out from this buffer, used to compute hold time at requeue.
        qint64 dqMonotonicNs = 0;
        quint64 traceSeq = 0;
    };

    // Newest compressed MJPEG payload handed from the capture thread to the decode worker (item 4).
    // The capture thread copies the payload (so the V4L2 buffer can be requeued immediately) and
    // overwrites any not-yet-decoded job — depth-one, newest wins.
    struct MjpegDecodeJob {
        std::vector<uint8_t> payload;
        int bytesUsed = 0;
        qint64 capturedAtNs = 0;
        qint64 wakeAtNs = 0;
        quint64 traceSeq = 0; // frame-trace join key (0 when tracing disabled)
        int captureBufferIndex = -1;
    };

    void handleReadyRead();
    void drainRequeuePending();
    [[nodiscard]] capture::DmaBufFrameHandle makeDmaBufFrameHandle(const v4l2_buffer &buffer);
    [[nodiscard]] capture::DmaBufFrameHandle makeVaapiMjpegDmaFrameHandle(
        const mjpeg_backend::VaapiDmaFrame &decoded,
        const MjpegDecodeJob &job);
    void startMjpegDecodeWorker();
    void stopMjpegDecodeWorker();
    void mjpegDecodeLoop();
    void decodeMjpegJob(MjpegDecodeJob &job);
    void queueMjpegForDecode(
        const uchar *payload,
        int bytesUsed,
        qint64 capturedAtNs,
        qint64 wakeAtNs,
        quint64 traceSeq,
        int captureBufferIndex);
    void requeueCaptureBuffer(int bufferIndex);
    void noteRequeueRequested(int bufferIndex);
    [[nodiscard]] bool useDmaBufDisplayPath() const;
    [[nodiscard]] bool useVaapiMjpegDmaPath() const;
    [[nodiscard]] static bool pixelFormatSupportsDmaBufDisplay(quint32 pixelFormat);
    [[nodiscard]] bool configureDevice(const capture::CaptureDevice &device, const capture::CaptureFormat &format);
    [[nodiscard]] bool allocateBuffers();
    [[nodiscard]] bool queueBuffers();
    [[nodiscard]] capture::FrameHandle decodeFrame(const void *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeYuyv(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeNv12(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeP010(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeI420(const uchar *data, int bytesUsed, bool yvu);
    [[nodiscard]] capture::FrameHandle decodeRgb24(const uchar *data, int bytesUsed, bool bgr, bool flipVertical);
    [[nodiscard]] capture::FrameHandle decodeMjpeg(const uchar *data, int bytesUsed);
    [[nodiscard]] QImage *writableFramePixels(const capture::FrameHandle &frame);
    void recordDecodedFrame(int bytesUsed, qint64 decodeNs, bool dmaBufPath);
    void releaseVaapiMjpegDmaResources(int vaapiSlotIndex);
    [[nodiscard]] bool frameReadyForDmaDisplay(int bytesUsed, __u32 bufferFlags) const;
    void cleanupBuffers();
    void closeDevice();

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int bytesPerLine_ = 0;
    quint32 pixelFormat_ = 0;
    double configuredFps_ = 0.0;
    bool streaming_ = false;
    std::vector<Buffer> buffers_;
    std::shared_ptr<capture::FrameBufferPool> framePool_;
    std::shared_ptr<std::vector<capture::DmaBufFrame>> dmaFramePool_;
    std::shared_ptr<std::vector<capture::DmaBufFrame>> vaapiMjpegDmaPool_;
    bool dmaBufExportSupported_ = false;
    bool dmaBufDisplayEnabled_ = false;
    bool vaapiMjpegDmaEnabled_ = false;
    std::atomic<bool> dmaBufDisplayRequested_ { false };
    std::shared_ptr<std::atomic<uint64_t>> requeuePending_;
    qint64 telemetryWindowStartNs_ = 0;
    int telemetryFrameCount_ = 0;
    qint64 telemetryDecodeTotalNs_ = 0;
    qint64 telemetryPayloadTotalBytes_ = 0;
    int telemetryDmaFrameCount_ = 0;
    int telemetryCpuFrameCount_ = 0;
    int telemetryMjpegHwFrameCount_ = 0;
    capture::MjpegDecodePath telemetryLastMjpegPath_ = capture::MjpegDecodePath::None;
    MjpegDecoder mjpegDecoder_;
    bool dmaBufHandleFailureLogged_ = false;
    bool dmaBufWarmupSkipLogged_ = false;
    QSocketNotifier *notifier_ = nullptr;

    // MJPEG VA-API decode worker (items 4-5). Decode (VA submit/sync/export) runs here instead of on
    // the capture/notifier thread, so it cannot delay the next V4L2 dequeue. The VA decoder is
    // initialized and destroyed on the capture thread while the worker is stopped, and only ever
    // *decoded* on the worker, so libva is never accessed concurrently.
    std::thread mjpegWorker_;
    std::mutex mjpegMutex_;
    std::condition_variable mjpegCv_;
    MjpegDecodeJob mjpegJob_;       // newest pending job (guarded by mjpegMutex_)
    MjpegDecodeJob mjpegWorkerJob_; // worker-local scratch, swapped under lock to reuse the buffer
    bool mjpegJobValid_ = false;    // guarded by mjpegMutex_
    bool mjpegWorkerStop_ = false;  // guarded by mjpegMutex_
    // Set by the worker if VA decode/export fails at runtime; the capture thread then falls back to the
    // CPU MJPEG path and the worker parks (so the decoder is never used from both threads at once).
    std::atomic<bool> vaapiMjpegDmaRuntimeFailed_ { false };

    // Frame-trace buffer-lifetime counters (CONSOLATION_FRAME_TRACE). All cross capture/UI threads.
    static constexpr std::size_t kTraceCaptureBufferSlots = 32;
    static constexpr std::size_t kTraceVaapiSlots = 8;
    // Capture-thread-only counter feeding DmaBufFrame::publishSeq / latestDmaSeq for UI coalescing.
    quint64 dmaPublishSeq_ = 0;
    std::atomic<int> outstandingCaptureBuffers_ { 0 };
    std::atomic<int> outstandingVaapiSlots_ { 0 };
    std::array<std::atomic<qint64>, kTraceCaptureBufferSlots> requeueRequestedNs_ {};
    std::array<std::atomic<qint64>, kTraceVaapiSlots> vaapiSlotHandoutNs_ {};
    std::array<std::atomic<quint64>, kTraceVaapiSlots> vaapiSlotSeq_ {};
};

} // namespace consolation::platform::linux
