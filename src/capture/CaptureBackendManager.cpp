#include "capture/CaptureBackendManager.h"

#include "platform/linux/V4L2CaptureSession.h"
#include "platform/linux/V4L2DeviceDiscovery.h"

namespace consolation::capture {

namespace {

bool isLikelyWebcamOrCamera(const CaptureDevice &device)
{
    const auto haystack = QStringList {
        device.displayName,
        device.devicePath,
        device.stableId,
        device.nodeName,
        device.v4l2DevicePath,
    }.join(QStringLiteral(" "));

    return haystack.contains(QStringLiteral("webcam"), Qt::CaseInsensitive) ||
        haystack.contains(QStringLiteral("camera"), Qt::CaseInsensitive);
}

} // namespace

std::vector<CaptureDevice> CaptureBackendManager::enumerateDevices() const
{
    std::vector<CaptureDevice> devices;

    for (auto device : platform::linux::V4L2DeviceDiscovery().enumerateDevices()) {
        if (isLikelyWebcamOrCamera(device)) {
            continue;
        }
        devices.push_back(std::move(device));
    }

    return devices;
}

std::unique_ptr<CaptureSession> CaptureBackendManager::createSession(const CaptureBackend backend) const
{
    switch (backend) {
    case CaptureBackend::V4L2:
        return std::make_unique<platform::linux::V4L2CaptureSession>();
    case CaptureBackend::Mock:
        return nullptr;
    }

    return nullptr;
}

} // namespace consolation::capture
