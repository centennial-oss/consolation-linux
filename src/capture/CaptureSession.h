#pragma once

#include "capture/CaptureTypes.h"

#include <QImage>
#include <QObject>

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

signals:
    void frameReady(const QImage &frame);
    void telemetryReady(const VideoTelemetrySnapshot &snapshot);
    void failed(const QString &message);
    void logMessage(const QString &message);
};

} // namespace consolation::capture
