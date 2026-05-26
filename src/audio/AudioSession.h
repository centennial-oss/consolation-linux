#pragma once

#include "capture/CaptureTypes.h"

namespace consolation::audio {

class AudioSession {
public:
    virtual ~AudioSession() = default;

    [[nodiscard]] virtual bool start(const capture::CaptureDevice &device, int volumePercent) = 0;
    virtual void stop() = 0;
    virtual void setVolumePercent(int volumePercent) = 0;
};

} // namespace consolation::audio
