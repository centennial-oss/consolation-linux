#pragma once

#include "capture/CaptureTypes.h"

#include <memory>
#include <vector>

namespace consolation::capture {
class CaptureSession;
}

namespace consolation::capture {

class CaptureBackendManager {
public:
    [[nodiscard]] std::vector<CaptureDevice> enumerateDevices() const;
    [[nodiscard]] std::unique_ptr<CaptureSession> createSession(CaptureBackend backend) const;
};

} // namespace consolation::capture
