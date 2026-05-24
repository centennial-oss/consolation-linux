#pragma once

#include "capture/CaptureSession.h"

#include <QImage>

namespace consolation::platform::linux {

class PipeWireCaptureSession final : public capture::CaptureSession {
    Q_OBJECT

public:
    explicit PipeWireCaptureSession(QObject *parent = nullptr);
    ~PipeWireCaptureSession() override;

    [[nodiscard]] bool start(const capture::CaptureDevice &device, const capture::CaptureFormat &format) override;
    void stop() override;

public:
    void iteratePipeWire();
    void handleStateChanged(int state, const char *error);
    void handleParamChanged(uint32_t id, const void *param);
    void handleProcess();
    [[nodiscard]] QImage imageFromBuffer(void *buffer) const;

private:
    void *threadLoop_ = nullptr;
    void *context_ = nullptr;
    void *core_ = nullptr;
    void *stream_ = nullptr;
    void *streamListener_ = nullptr;
    void *videoInfo_ = nullptr;
    bool started_ = false;
};

} // namespace consolation::platform::linux
