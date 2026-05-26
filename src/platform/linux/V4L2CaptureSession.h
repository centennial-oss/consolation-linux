#pragma once

#include "capture/CaptureSession.h"
#include "capture/FrameBufferPool.h"

#include <QSocketNotifier>

#include <linux/videodev2.h>

#include <atomic>
#include <memory>
#include <vector>

namespace consolation::platform::linux {

class V4L2CaptureSession final : public capture::CaptureSession {
    Q_OBJECT

public:
    explicit V4L2CaptureSession(QObject *parent = nullptr);
    ~V4L2CaptureSession() override;

    [[nodiscard]] bool start(const capture::CaptureDevice &device, const capture::CaptureFormat &format) override;
    void stop() override;

    void setDmaBufDisplayRequested(const bool requested);

    Q_INVOKABLE void finishDmaFrameAsCpu(capture::DmaBufFrameHandle frame);

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
        int dmaFd = -1;
    };

    void handleReadyRead();
    [[nodiscard]] capture::DmaBufFrameHandle makeDmaBufFrameHandle(const v4l2_buffer &buffer);
    Q_INVOKABLE void requeueCaptureBuffer(int bufferIndex);
    [[nodiscard]] bool useDmaBufDisplayPath() const;
    [[nodiscard]] bool configureDevice(const capture::CaptureDevice &device, const capture::CaptureFormat &format);
    [[nodiscard]] bool allocateBuffers();
    [[nodiscard]] bool queueBuffers();
    [[nodiscard]] capture::FrameHandle decodeFrame(const void *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeYuyv(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeNv12(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeI420(const uchar *data, int bytesUsed, bool yvu);
    [[nodiscard]] capture::FrameHandle decodeRgb24(const uchar *data, int bytesUsed, bool bgr, bool flipVertical);
    [[nodiscard]] capture::FrameHandle decodeMjpeg(const uchar *data, int bytesUsed);
    [[nodiscard]] capture::FrameHandle decodeMjpegQtFallback(const uchar *data, int bytesUsed);
    [[nodiscard]] QImage *writableFramePixels(const capture::FrameHandle &frame);
    void recordDecodedFrame(int bytesUsed, qint64 decodeNs);
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
    bool dmaBufExportSupported_ = false;
    bool dmaBufDisplayEnabled_ = false;
    std::atomic<bool> dmaBufDisplayRequested_ { false };
    qint64 telemetryWindowStartNs_ = 0;
    int telemetryFrameCount_ = 0;
    qint64 telemetryDecodeTotalNs_ = 0;
    qint64 telemetryDecodeMaxNs_ = 0;
    qint64 telemetryPayloadTotalBytes_ = 0;
    QSocketNotifier *notifier_ = nullptr;
};

} // namespace consolation::platform::linux
