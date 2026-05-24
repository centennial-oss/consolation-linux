#include "capture/CaptureBackendManager.h"
#include "capture/CaptureSession.h"

#include <QCoreApplication>
#include <QTimer>
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
    QTextStream err(stderr);

    consolation::capture::CaptureBackendManager manager;
    const auto devices = manager.enumerateDevices();
    out << "Discovered " << devices.size() << " device(s)\n";
    for (qsizetype index = 0; index < static_cast<qsizetype>(devices.size()); ++index) {
        const auto &device = devices[static_cast<size_t>(index)];
        out << "[" << index << "] " << backendName(device.backend) << " "
            << device.displayName << " " << device.devicePath
            << " stableId=" << device.stableId
            << " nodeId=" << device.backendNodeId
            << " formats=" << device.formats.size() << "\n";
        for (const auto &format : device.formats) {
            out << "    " << format.label << "\n";
        }
    }
    out.flush();

    if (devices.empty()) {
        err << "No capture devices found.\n";
        return 2;
    }

    const auto &device = devices.front();
    if (device.formats.empty()) {
        err << "Selected device has no formats.\n";
        return 3;
    }

    auto session = manager.createSession(device.backend);
    if (!session) {
        err << "No session implementation for backend " << backendName(device.backend) << "\n";
        return 4;
    }

    int frames = 0;
    int exitCode = 1;
    QObject::connect(session.get(), &consolation::capture::CaptureSession::logMessage, &app, [&](const QString &message) {
        out << "[capture] " << message << "\n";
        out.flush();
    });
    QObject::connect(session.get(), &consolation::capture::CaptureSession::frameReady, &app, [&](const QImage &frame) {
        ++frames;
        out << "[frame] " << frames << " " << frame.width() << "x" << frame.height() << " format=" << frame.format() << "\n";
        out.flush();
        if (frames >= 5) {
            exitCode = 0;
            session->stop();
            app.quit();
        }
    });
    QObject::connect(session.get(), &consolation::capture::CaptureSession::failed, &app, [&](const QString &message) {
        err << "[failed] " << message << "\n";
        err.flush();
        exitCode = 5;
        app.quit();
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &app, [&]() {
        err << "[failed] timed out waiting for frames\n";
        err.flush();
        exitCode = 6;
        session->stop();
        app.quit();
    });
    timeout.start(10000);

    out << "Starting first device with first format\n";
    out.flush();
    if (!session->start(device, device.formats.front())) {
        return 7;
    }

    app.exec();
    session->stop();
    return exitCode;
}
