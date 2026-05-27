#include "capture/FrameBufferPool.h"

#include <algorithm>

namespace consolation::capture {

FrameBufferPool::FrameBufferPool(const size_t poolSize)
    : poolSize_(poolSize > 0 ? std::min(poolSize, maxPoolSlots) : defaultPoolSize)
{
}

FrameHandle FrameBufferPool::acquireForDecode(const int width, const int height, const QImage::Format format)
{
    if (width <= 0 || height <= 0 || format == QImage::Format_Invalid) {
        return {};
    }

    {
        std::lock_guard lock(geometryMutex_);
        if (slots_.size() != poolSize_) {
            slots_.assign(poolSize_, {});
            inUseMask_.store(0, std::memory_order_release);
            nextIndex_.store(0, std::memory_order_relaxed);
            cachedWidth_ = 0;
            cachedHeight_ = 0;
            cachedFormat_ = QImage::Format_Invalid;
        }

        ensureGeometryLocked(width, height, format);
    }

    const auto index = claimSlotIndex();
    if (index >= slots_.size()) {
        return {};
    }

    return makeHandle(index);
}

void FrameBufferPool::reset()
{
    std::lock_guard lock(geometryMutex_);
    for (auto &slot : slots_) {
        slot.image = {};
    }
    inUseMask_.store(0, std::memory_order_release);
    nextIndex_.store(0, std::memory_order_relaxed);
    cachedWidth_ = 0;
    cachedHeight_ = 0;
    cachedFormat_ = QImage::Format_Invalid;
}

void FrameBufferPool::releaseSlot(const size_t index)
{
    if (index >= maxPoolSlots) {
        return;
    }
    inUseMask_.fetch_and(~(uint64_t{1} << index), std::memory_order_release);
}

void FrameBufferPool::ensureGeometryLocked(const int width, const int height, const QImage::Format format)
{
    if (cachedWidth_ == width && cachedHeight_ == height && cachedFormat_ == format) {
        return;
    }

    for (auto &slot : slots_) {
        slot.image = QImage(width, height, format);
    }

    cachedWidth_ = width;
    cachedHeight_ = height;
    cachedFormat_ = format;
    inUseMask_.store(0, std::memory_order_release);
    nextIndex_.store(0, std::memory_order_relaxed);
}

size_t FrameBufferPool::claimSlotIndex()
{
    const auto poolSize = slots_.size();
    if (poolSize == 0) {
        return 0;
    }

    const auto allInUseMask = poolSize >= maxPoolSlots ? ~uint64_t{0} : ((uint64_t{1} << poolSize) - 1);

    while (true) {
        auto mask = inUseMask_.load(std::memory_order_acquire);
        if ((mask & allInUseMask) == allInUseMask) {
            return poolSize;
        }

        const auto start = nextIndex_.fetch_add(1, std::memory_order_relaxed) % poolSize;
        for (size_t attempt = 0; attempt < poolSize; ++attempt) {
            const auto index = (start + attempt) % poolSize;
            const auto bit = uint64_t{1} << index;
            if ((mask & bit) != 0) {
                continue;
            }

            const auto desired = mask | bit;
            if (inUseMask_.compare_exchange_weak(mask, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return index;
            }
            break;
        }
    }
}

FrameHandle FrameBufferPool::makeHandle(const size_t index)
{
    auto self = shared_from_this();
    const auto *image = &slots_[index].image;
    return FrameHandle(image, [self, index](const QImage *) { self->releaseSlot(index); });
}

} // namespace consolation::capture
