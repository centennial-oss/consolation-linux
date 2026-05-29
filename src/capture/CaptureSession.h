#pragma once

#include "capture/CaptureTypes.h"
#include "capture/DmaBufFrame.h"

#include <QObject>

#include <atomic>

namespace consolation::capture {

class CaptureSession : public QObject {
    Q_OBJECT

public:
    explicit CaptureSession(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~CaptureSession() override = default;

    [[nodiscard]] virtual bool start(const CaptureDevice &device, const CaptureFormat &format) = 0;
    virtual void stop() = 0;

    // Monotonic id of the newest dma frame published via dmaFrameReady. Producers store the
    // outgoing frame's publishSeq here immediately before emitting; the UI thread drops any
    // delivered frame older than this value (a newer frame is already in its queue). Written on
    // the capture thread, read on the UI thread.
    std::atomic<quint64> latestDmaSeq { 0 };

signals:
    void frameReady(FrameHandle frame, qint64 capturedAtNs, qint64 wakeAtNs);
    void dmaFrameReady(DmaBufFrameHandle frame);
    void telemetryReady(const VideoTelemetrySnapshot &snapshot);
    void failed(const QString &message);
    void logMessage(const QString &message);
};

} // namespace consolation::capture
