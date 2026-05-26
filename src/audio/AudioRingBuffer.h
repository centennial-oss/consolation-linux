#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace consolation::audio {

class AudioRingBuffer final {
public:
    AudioRingBuffer(size_t capacityFrames, int channels);

    void clear();
    void write(const float *samples, size_t frames);
    size_t read(float *samples, size_t frames, float gain);
    [[nodiscard]] size_t availableFrames() const;

private:
    [[nodiscard]] size_t frameOffset(size_t frameIndex) const;

    const size_t capacityFrames_;
    const int channels_;
    std::vector<float> samples_;
    std::atomic<size_t> readFrame_ { 0 };
    std::atomic<size_t> writeFrame_ { 0 };
};

} // namespace consolation::audio
