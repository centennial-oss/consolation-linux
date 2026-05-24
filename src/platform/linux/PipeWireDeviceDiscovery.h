#pragma once

#include "capture/CaptureTypes.h"

#include <vector>

namespace consolation::platform::linux {

class PipeWireDeviceDiscovery {
public:
    [[nodiscard]] std::vector<capture::CaptureDevice> enumerateDevices() const;
    [[nodiscard]] QString resolveTargetObject(const capture::CaptureDevice &device) const;
};

} // namespace consolation::platform::linux
