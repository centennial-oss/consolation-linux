#include "capture/CaptureBackendManager.h"

#include "platform/linux/PipeWireCaptureSession.h"
#include "platform/linux/PipeWireDeviceDiscovery.h"
#include "platform/linux/V4L2CaptureSession.h"
#include "platform/linux/V4L2DeviceDiscovery.h"

#include <QSet>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace consolation::capture {

namespace {

bool sharesV4L2Path(const CaptureDevice &left, const CaptureDevice &right)
{
    if (left.v4l2DevicePath.isEmpty() || right.v4l2DevicePath.isEmpty()) {
        return false;
    }
    return left.v4l2DevicePath == right.v4l2DevicePath;
}

bool canOpenV4L2Capture(const QString &path)
{
    const auto fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    int input = 0;
    const bool available = ::ioctl(fd, VIDIOC_G_INPUT, &input) == 0;
    ::close(fd);
    if (!available && errno == EBUSY) {
        return false;
    }
    return available;
}

} // namespace

std::vector<CaptureDevice> CaptureBackendManager::enumerateDevices() const
{
    std::vector<CaptureDevice> devices;
    QSet<QString> servedV4L2Paths;

    for (auto device : platform::linux::V4L2DeviceDiscovery().enumerateDevices()) {
        if (!canOpenV4L2Capture(device.devicePath)) {
            continue;
        }

        servedV4L2Paths.insert(device.devicePath);
        devices.push_back(std::move(device));
    }

    for (const auto &pipewireDevice : platform::linux::PipeWireDeviceDiscovery().enumerateDevices()) {
        if (!pipewireDevice.v4l2DevicePath.isEmpty() && servedV4L2Paths.contains(pipewireDevice.v4l2DevicePath)) {
            // Prefer direct V4L2 when available. PipeWire's v4l2 source can trigger
            // USB reset/reconnect during aggressive format negotiation.
            continue;
        }

        const auto duplicatesExisting = std::any_of(devices.begin(), devices.end(), [&](const CaptureDevice &existing) {
            return sharesV4L2Path(existing, pipewireDevice);
        });
        if (!duplicatesExisting) {
            devices.push_back(pipewireDevice);
        }
    }

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
