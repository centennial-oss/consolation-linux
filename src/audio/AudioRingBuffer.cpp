#include "audio/AudioRingBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace consolation::audio {

namespace {

constexpr float unityGainEpsilon = 0.000'1F;

} // namespace

AudioRingBuffer::AudioRingBuffer(const size_t capacityFrames, const int channels)
    : capacityFrames_(capacityFrames)
    , channels_(channels)
    , samples_(capacityFrames * static_cast<size_t>(channels), 0.0F)
{
}

void AudioRingBuffer::clear()
{
    readFrame_.store(0, std::memory_order_release);
    writeFrame_.store(0, std::memory_order_release);
}

void AudioRingBuffer::write(const float *samples, const size_t frames)
{
    if (samples == nullptr || frames == 0 || capacityFrames_ == 0) {
        return;
    }

    auto readFrame = readFrame_.load(std::memory_order_acquire);
    auto writeFrame = writeFrame_.load(std::memory_order_relaxed);
    const auto writableFrames = std::min(frames, capacityFrames_);
    const auto droppedInputFrames = frames - writableFrames;
    const auto *source = samples + droppedInputFrames * static_cast<size_t>(channels_);

    if (writeFrame + writableFrames - readFrame > capacityFrames_) {
        const auto targetReadFrame = writeFrame + writableFrames - capacityFrames_;
        auto currentReadFrame = readFrame;
        while (currentReadFrame < targetReadFrame &&
               !readFrame_.compare_exchange_weak(
                   currentReadFrame,
                   targetReadFrame,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
    }

    auto remaining = writableFrames;
    while (remaining > 0) {
        const auto offset = frameOffset(writeFrame);
        const auto contiguous = std::min(remaining, capacityFrames_ - offset);
        std::memcpy(
            samples_.data() + offset * static_cast<size_t>(channels_),
            source,
            contiguous * static_cast<size_t>(channels_) * sizeof(float));
        source += contiguous * static_cast<size_t>(channels_);
        writeFrame += contiguous;
        remaining -= contiguous;
    }

    writeFrame_.store(writeFrame, std::memory_order_release);
}

size_t AudioRingBuffer::read(float *samples, const size_t frames, const float gain)
{
    if (samples == nullptr || frames == 0 || capacityFrames_ == 0) {
        return 0;
    }

    auto readFrame = readFrame_.load(std::memory_order_relaxed);
    const auto writeFrame = writeFrame_.load(std::memory_order_acquire);
    const auto readableFrames = std::min(frames, writeFrame - readFrame);
    const auto unityGain = std::fabs(gain - 1.0F) <= unityGainEpsilon;

    auto remaining = readableFrames;
    auto *destination = samples;
    while (remaining > 0) {
        const auto offset = frameOffset(readFrame);
        const auto contiguous = std::min(remaining, capacityFrames_ - offset);
        const auto sampleCount = contiguous * static_cast<size_t>(channels_);
        const auto *source = samples_.data() + offset * static_cast<size_t>(channels_);
        if (unityGain) {
            std::memcpy(destination, source, sampleCount * sizeof(float));
        } else {
            for (auto index = 0U; index < sampleCount; ++index) {
                destination[index] = source[index] * gain;
            }
        }
        destination += sampleCount;
        readFrame += contiguous;
        remaining -= contiguous;
    }

    readFrame_.store(readFrame, std::memory_order_release);

    const auto underrunFrames = frames - readableFrames;
    if (underrunFrames > 0) {
        std::fill(
            samples + readableFrames * static_cast<size_t>(channels_),
            samples + frames * static_cast<size_t>(channels_),
            0.0F);
    }

    return readableFrames;
}

size_t AudioRingBuffer::availableFrames() const
{
    const auto readFrame = readFrame_.load(std::memory_order_acquire);
    const auto writeFrame = writeFrame_.load(std::memory_order_acquire);
    return writeFrame - readFrame;
}

size_t AudioRingBuffer::frameOffset(const size_t frameIndex) const
{
    return frameIndex % capacityFrames_;
}

} // namespace consolation::audio
