#pragma once

#include "capture/CaptureSession.h"

#include <QSocketNotifier>

#include <vector>

namespace consolation::platform::linux {

class V4L2CaptureSession final : public capture::CaptureSession {
    Q_OBJECT

public:
    explicit V4L2CaptureSession(QObject *parent = nullptr);
    ~V4L2CaptureSession() override;

    [[nodiscard]] bool start(const capture::CaptureDevice &device, const capture::CaptureFormat &format) override;
    void stop() override;

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };

    void handleReadyRead();
    [[nodiscard]] bool configureDevice(const capture::CaptureDevice &device, const capture::CaptureFormat &format);
    [[nodiscard]] bool allocateBuffers();
    [[nodiscard]] bool queueBuffers();
    [[nodiscard]] QImage decodeFrame(const void *data, int bytesUsed) const;
    [[nodiscard]] QImage decodeYuyv(const uchar *data, int bytesUsed) const;
    [[nodiscard]] QImage decodeNv12(const uchar *data, int bytesUsed) const;
    void cleanupBuffers();
    void closeDevice();

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int bytesPerLine_ = 0;
    quint32 pixelFormat_ = 0;
    bool streaming_ = false;
    std::vector<Buffer> buffers_;
    QSocketNotifier *notifier_ = nullptr;
};

} // namespace consolation::platform::linux
