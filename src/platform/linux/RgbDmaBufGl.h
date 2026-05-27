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

    [[nodiscard]] QString lastBindFailure() const
    {
        return lastBindFailure_;
    }

private:
    struct SlotBinding {
        int dmaFd = -1;
        capture::DmaBufLayout layout = capture::DmaBufLayout::Unknown;
        int byteWidth = 0;
        void *eglImage = nullptr;
        unsigned int textureId = 0;
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
    unsigned int vaoId_ = 0;
    int frameUniform_ = -1;
    int pixelWidthUniform_ = -1;
    int byteWidthUniform_ = -1;
    int bgrUniform_ = -1;
    int flipUniform_ = -1;
    int activeSlot_ = -1;
    bool boundBgr_ = false;
    bool boundFlipVertical_ = false;
    float boundPixelWidth_ = 0.0F;
    float boundByteWidth_ = 0.0F;
    // Dirty-flag cache: tracks last values sent to per-slot uniforms to skip redundant glUniform calls.
    float lastSentPixelWidth_ = -1.0F;
    float lastSentByteWidth_ = -1.0F;
    int lastSentBgr_ = -1;
    int lastSentFlipY_ = -1;
    capture::DmaBufFrameHandle boundFrame_;
    std::array<SlotBinding, maxBufferSlots> slots_ {};
};

} // namespace consolation::platform::linux
