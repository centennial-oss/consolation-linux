#pragma once

#include "capture/DmaBufFrame.h"

#include <QOpenGLFunctions>
#include <QString>
#include <QSize>

#include <array>

namespace consolation::platform::linux {

// Imports planar I420/YV12 dma-buf as three R8 EGLImages (Y, U/Cb, V/Cr) and draws with a YUV shader.
class I420DmaBufGl final : protected QOpenGLFunctions {
public:
    static constexpr int maxBufferSlots = 64;

    bool initialize();
    void shutdown();

    [[nodiscard]] bool bindFrame(const capture::DmaBufFrameHandle &frame);
    void releaseFrame();
    void releaseAllSlots();

    void draw(const QSize &widgetSize, const QRect &targetRect, float devicePixelRatio);

    [[nodiscard]] bool isAvailable() const
    {
        return available_;
    }

    [[nodiscard]] QString lastInitFailure() const
    {
        return lastInitFailure_;
    }

    [[nodiscard]] QString lastBindFailure() const
    {
        return lastBindFailure_;
    }

private:
    struct SlotBinding {
        int dmaFd = -1;
        capture::DmaBufLayout layout = capture::DmaBufLayout::Unknown;
        void *eglImageY = nullptr;
        void *eglImageU = nullptr;
        void *eglImageV = nullptr;
        unsigned int yTextureId = 0;
        unsigned int uTextureId = 0;
        unsigned int vTextureId = 0;
    };

    [[nodiscard]] bool resolveEglDisplay();
    [[nodiscard]] bool resolveExtensions();
    void releaseSlot(int bufferIndex);
    [[nodiscard]] bool ensureSlotBound(const capture::DmaBufFrameHandle &frame, int bufferIndex);

    bool available_ = false;
    QString lastInitFailure_;
    QString lastBindFailure_;
    void *eglDisplay_ = nullptr;
    unsigned int programId_ = 0;
    unsigned int vboId_ = 0;
    int yUniform_ = -1;
    int uUniform_ = -1;
    int vUniform_ = -1;
    int activeSlot_ = -1;
    capture::DmaBufFrameHandle boundFrame_;
    std::array<SlotBinding, maxBufferSlots> slots_ {};
};

} // namespace consolation::platform::linux
