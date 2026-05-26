#pragma once

#include "capture/CaptureTypes.h"

#include <QImage>

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace consolation::capture {

// Reusable RGB decode buffers. FrameHandle uses a custom deleter to return slots when the
// last consumer drops its reference (any thread).
class FrameBufferPool : public std::enable_shared_from_this<FrameBufferPool> {
public:
    static constexpr size_t defaultPoolSize = 10;

    explicit FrameBufferPool(size_t poolSize = defaultPoolSize);

    [[nodiscard]] FrameHandle acquireForDecode(int width, int height, QImage::Format format);

    void reset();

private:
    struct Slot {
        QImage image;
        bool inUse = false;
    };

    void releaseSlot(size_t index);
    void ensureGeometryLocked(int width, int height, QImage::Format format);
    [[nodiscard]] size_t findFreeSlotLocked();
    [[nodiscard]] FrameHandle makeHandleLocked(size_t index);
    [[nodiscard]] static FrameHandle heapFallback(int width, int height, QImage::Format format);

    size_t poolSize_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    size_t nextIndex_ = 0;
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
    QImage::Format cachedFormat_ = QImage::Format_Invalid;
};

} // namespace consolation::capture
