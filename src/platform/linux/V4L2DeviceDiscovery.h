#pragma once

#include "capture/CaptureTypes.h"

#include <vector>

namespace consolation::platform::linux {

class V4L2DeviceDiscovery {
public:
    [[nodiscard]] std::vector<capture::CaptureDevice> enumerateDevices() const;
};

} // namespace consolation::platform::linux
