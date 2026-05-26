#pragma once

#include "capture/DmaBufFrame.h"

#include <QOpenGLFunctions>
#include <QString>
#include <QSize>

#include <array>

namespace consolation::platform::linux {

// Imports packed RGB/BGR dma-buf (24 bpp) as an EGLImage and draws with a textured quad.
class RgbDmaBufGl final : protected QOpenGLFunctions {
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

private:
    struct SlotBinding {
        int dmaFd = -1;
        capture::DmaBufLayout layout = capture::DmaBufLayout::Unknown;
        void *eglImage = nullptr;
        unsigned int textureId = 0;
    };

    [[nodiscard]] bool resolveEglDisplay();
    [[nodiscard]] bool resolveExtensions();
    void releaseSlot(int bufferIndex);
    [[nodiscard]] bool ensureSlotBound(const capture::DmaBufFrameHandle &frame, int bufferIndex);

    bool available_ = false;
    QString lastInitFailure_;
    void *eglDisplay_ = nullptr;
    unsigned int programId_ = 0;
    unsigned int vboId_ = 0;
    int frameUniform_ = -1;
    int bgrUniform_ = -1;
    int flipUniform_ = -1;
    int activeSlot_ = -1;
    bool boundBgr_ = false;
    bool boundFlipVertical_ = false;
    capture::DmaBufFrameHandle boundFrame_;
    std::array<SlotBinding, maxBufferSlots> slots_ {};
};

} // namespace consolation::platform::linux
