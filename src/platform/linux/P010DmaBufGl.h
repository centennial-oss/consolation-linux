#pragma once

#include "capture/DmaBufFrame.h"

#include <QOpenGLFunctions>
#include <QString>
#include <QSize>

#include <array>

namespace consolation::platform::linux {

// Imports P010 dma-buf planes as EGLImages (R16 + GR1616) and draws with a YUV shader.
class P010DmaBufGl final : protected QOpenGLFunctions {
public:
    static constexpr int maxBufferSlots = 64;

    bool initialize();
    void shutdown();

    [[nodiscard]] bool bindFrame(const capture::DmaBufFrameHandle &frame);
    void releaseFrame();
    void releaseAllSlots();
    void invalidateSlot(int bufferIndex);

    void draw(const QSize &widgetSize, const QRect &targetRect, float devicePixelRatio);

    [[nodiscard]] bool isAvailable() const
    {
        return available_;
    }

    [[nodiscard]] QString lastInitFailure() const
    {
        return lastInitFailure_;
    }

private:
    struct SlotBinding {
        int dmaFd = -1;
        void *eglImageY = nullptr;
        void *eglImageUv = nullptr;
        unsigned int yTextureId = 0;
        unsigned int uvTextureId = 0;
    };

    [[nodiscard]] bool resolveEglDisplay();
    [[nodiscard]] bool resolveExtensions();
    void releaseSlot(const int bufferIndex);
    [[nodiscard]] bool ensureSlotBound(const capture::DmaBufFrameHandle &frame, int bufferIndex);

    bool available_ = false;
    QString lastInitFailure_;
    void *eglDisplay_ = nullptr;
    unsigned int programId_ = 0;
    unsigned int vboId_ = 0;
    unsigned int vaoId_ = 0;
    int yUniform_ = -1;
    int uvUniform_ = -1;
    int activeSlot_ = -1;
    capture::DmaBufFrameHandle boundFrame_;
    std::array<SlotBinding, maxBufferSlots> slots_ {};
};

} // namespace consolation::platform::linux
