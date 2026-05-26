#include "capture/FrameBufferPool.h"

namespace consolation::capture {

FrameBufferPool::FrameBufferPool(const size_t poolSize)
    : poolSize_(poolSize > 0 ? poolSize : defaultPoolSize)
{
}

FrameHandle FrameBufferPool::acquireForDecode(const int width, const int height, const QImage::Format format)
{
    if (width <= 0 || height <= 0 || format == QImage::Format_Invalid) {
        return {};
    }

    std::lock_guard lock(mutex_);
    if (slots_.size() != poolSize_) {
        slots_.assign(poolSize_, {});
        nextIndex_ = 0;
        cachedWidth_ = 0;
        cachedHeight_ = 0;
        cachedFormat_ = QImage::Format_Invalid;
    }

    ensureGeometryLocked(width, height, format);

    const auto index = findFreeSlotLocked();
    if (index >= slots_.size()) {
        return heapFallback(width, height, format);
    }

    slots_[index].inUse = true;
    return makeHandleLocked(index);
}

void FrameBufferPool::reset()
{
    std::lock_guard lock(mutex_);
    for (auto &slot : slots_) {
        slot.inUse = false;
        slot.image = {};
    }
    nextIndex_ = 0;
    cachedWidth_ = 0;
    cachedHeight_ = 0;
    cachedFormat_ = QImage::Format_Invalid;
}

void FrameBufferPool::releaseSlot(const size_t index)
{
    std::lock_guard lock(mutex_);
    if (index < slots_.size()) {
        slots_[index].inUse = false;
    }
}

void FrameBufferPool::ensureGeometryLocked(const int width, const int height, const QImage::Format format)
{
    if (cachedWidth_ == width && cachedHeight_ == height && cachedFormat_ == format) {
        return;
    }

    for (auto &slot : slots_) {
        slot.image = QImage(width, height, format);
        slot.inUse = false;
    }

    cachedWidth_ = width;
    cachedHeight_ = height;
    cachedFormat_ = format;
    nextIndex_ = 0;
}

size_t FrameBufferPool::findFreeSlotLocked()
{
    for (size_t attempt = 0; attempt < slots_.size(); ++attempt) {
        const auto index = (nextIndex_ + attempt) % slots_.size();
        if (!slots_[index].inUse) {
            nextIndex_ = (index + 1) % slots_.size();
            return index;
        }
    }
    return slots_.size();
}

FrameHandle FrameBufferPool::makeHandleLocked(const size_t index)
{
    auto self = shared_from_this();
    const auto *image = &slots_[index].image;
    return FrameHandle(image, [self, index](const QImage *) { self->releaseSlot(index); });
}

FrameHandle FrameBufferPool::heapFallback(const int width, const int height, const QImage::Format format)
{
    return std::make_shared<QImage>(width, height, format);
}

} // namespace consolation::capture
