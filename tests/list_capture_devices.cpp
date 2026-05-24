#include "capture/CaptureBackendManager.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {

QString backendName(const consolation::capture::CaptureBackend backend)
{
    switch (backend) {
    case consolation::capture::CaptureBackend::Mock:
        return QStringLiteral("mock");
    case consolation::capture::CaptureBackend::PipeWire:
        return QStringLiteral("pipewire");
    case consolation::capture::CaptureBackend::V4L2:
        return QStringLiteral("v4l2");
    }
    return QStringLiteral("unknown");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const auto devices = consolation::capture::CaptureBackendManager().enumerateDevices();
    for (const auto &device : devices) {
        out << backendName(device.backend) << " "
            << device.displayName << " "
            << device.devicePath << " formats=" << device.formats.size() << "\n";
        for (const auto &format : device.formats) {
            out << "  " << format.label << "\n";
        }
    }

    return 0;
}
