#pragma once

#include "capture/CaptureTypes.h"

#include <QImage>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace consolation::capture {

// Reusable RGB decode buffers. FrameHandle uses a custom deleter to return slots when the
// last consumer drops its reference (any thread).
//
// Slot claim (capture thread) and release (UI thread) use an atomic in-use bitmask; a mutex
// covers only pool init, reset, and geometry changes.
class FrameBufferPool : public std::enable_shared_from_this<FrameBufferPool> {
public:
    static constexpr size_t defaultPoolSize = 6;
    static constexpr size_t maxPoolSlots = 64;

    explicit FrameBufferPool(size_t poolSize = defaultPoolSize);

    [[nodiscard]] FrameHandle acquireForDecode(int width, int height, QImage::Format format);

    void reset();

private:
    struct Slot {
        QImage image;
    };

    void releaseSlot(size_t index);
    void ensureGeometryLocked(int width, int height, QImage::Format format);
    [[nodiscard]] size_t claimSlotIndex();
    [[nodiscard]] FrameHandle makeHandle(size_t index);

    size_t poolSize_;
    std::mutex geometryMutex_;
    std::vector<Slot> slots_;
    std::atomic<uint64_t> inUseMask_{0};
    std::atomic<size_t> nextIndex_{0};
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
    QImage::Format cachedFormat_ = QImage::Format_Invalid;
};

} // namespace consolation::capture
