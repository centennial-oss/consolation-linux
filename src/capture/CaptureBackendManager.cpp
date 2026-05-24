#include "capture/CaptureBackendManager.h"

#include "platform/linux/PipeWireCaptureSession.h"
#include "platform/linux/PipeWireDeviceDiscovery.h"
#include "platform/linux/V4L2CaptureSession.h"
#include "platform/linux/V4L2DeviceDiscovery.h"

namespace consolation::capture {

std::vector<CaptureDevice> CaptureBackendManager::enumerateDevices() const
{
    auto devices = platform::linux::PipeWireDeviceDiscovery().enumerateDevices();
    if (!devices.empty()) {
        return devices;
    }

    devices = platform::linux::V4L2DeviceDiscovery().enumerateDevices();
    if (!devices.empty()) {
        return devices;
    }

    CaptureDevice mockDevice;
    mockDevice.backend = CaptureBackend::Mock;
    mockDevice.devicePath = QStringLiteral("mock://capture-card");
    mockDevice.displayName = QStringLiteral("Mock Capture Device");
    mockDevice.stableId = QStringLiteral("mock-capture-card");
    mockDevice.formats.push_back(CaptureFormat{
        .width = 1920,
        .height = 1080,
        .framesPerSecond = 60.0,
        .pixelFormat = QStringLiteral("NV12"),
        .label = QStringLiteral("1920x1080 @ 60p · NV12"),
    });
    return {mockDevice};
}

std::unique_ptr<CaptureSession> CaptureBackendManager::createSession(const CaptureBackend backend) const
{
    switch (backend) {
    case CaptureBackend::V4L2:
        return std::make_unique<platform::linux::V4L2CaptureSession>();
    case CaptureBackend::PipeWire:
        return std::make_unique<platform::linux::PipeWireCaptureSession>();
    case CaptureBackend::Mock:
        return nullptr;
    }

    return nullptr;
}

} // namespace consolation::capture
