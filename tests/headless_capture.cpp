#include "capture/CaptureBackendManager.h"
#include "capture/CaptureSession.h"
#include "capture/CaptureTypes.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>
#include <QTextStream>

namespace {

QString backendName(const consolation::capture::CaptureBackend backend)
{
    switch (backend) {
    case consolation::capture::CaptureBackend::Mock:
        return QStringLiteral("mock");
    case consolation::capture::CaptureBackend::V4L2:
        return QStringLiteral("v4l2");
    }
    return QStringLiteral("unknown");
}

} // namespace

int main(int argc, char *argv[])
{
    qRegisterMetaType<consolation::capture::FrameHandle>();

    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Headless Consolation capture smoke runner"));
    parser.addHelpOption();
    const QCommandLineOption backendOption(
        QStringLiteral("backend"),
        QStringLiteral("Preferred backend: v4l2 or mock."),
        QStringLiteral("backend"));
    const QCommandLineOption formatOption(
        QStringLiteral("format-index"),
        QStringLiteral("Format index to use from the selected device."),
        QStringLiteral("index"),
        QStringLiteral("0"));
    const QCommandLineOption containsOption(
        QStringLiteral("format-contains"),
        QStringLiteral("First format whose label contains this text."),
        QStringLiteral("text"));
    parser.addOption(backendOption);
    parser.addOption(formatOption);
    parser.addOption(containsOption);
    parser.process(app);

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
            << " v4l2=" << device.v4l2DevicePath
            << " nodeName=" << device.nodeName
            << " formats=" << device.formats.size() << "\n";
        for (auto formatIndex = 0; formatIndex < static_cast<int>(device.formats.size()); ++formatIndex) {
            const auto &format = device.formats[static_cast<size_t>(formatIndex)];
            out << "    [" << formatIndex << "] " << format.label << "\n";
        }
    }
    out.flush();

    if (devices.empty()) {
        err << "No capture devices found.\n";
        return 2;
    }

    auto selectedDeviceIndex = 0;
    const auto requestedBackend = parser.value(backendOption).toLower();
    if (!requestedBackend.isEmpty()) {
        selectedDeviceIndex = -1;
        for (auto index = 0; index < static_cast<int>(devices.size()); ++index) {
            if (backendName(devices[static_cast<size_t>(index)].backend) == requestedBackend) {
                selectedDeviceIndex = index;
                break;
            }
        }
        if (selectedDeviceIndex < 0) {
            err << "No device found for backend " << requestedBackend << ".\n";
            return 8;
        }
    }

    const auto &device = devices[static_cast<size_t>(selectedDeviceIndex)];
    if (device.formats.empty()) {
        err << "Selected device has no formats.\n";
        return 3;
    }

    bool formatOk = false;
    auto selectedFormatIndex = parser.value(formatOption).toInt(&formatOk);
    if (!formatOk) {
        err << "Invalid --format-index value.\n";
        return 9;
    }
    const auto formatContains = parser.value(containsOption);
    if (!formatContains.isEmpty()) {
        selectedFormatIndex = -1;
        for (auto index = 0; index < static_cast<int>(device.formats.size()); ++index) {
            if (device.formats[static_cast<size_t>(index)].label.contains(formatContains, Qt::CaseInsensitive)) {
                selectedFormatIndex = index;
                break;
            }
        }
        if (selectedFormatIndex < 0) {
            err << "No format contains " << formatContains << ".\n";
            return 10;
        }
    }
    if (selectedFormatIndex < 0 || selectedFormatIndex >= static_cast<int>(device.formats.size())) {
        err << "Format index out of range.\n";
        return 9;
    }

    int frames = 0;
    int exitCode = 1;
    std::unique_ptr<consolation::capture::CaptureSession> session;
    const auto attachSession = [&](consolation::capture::CaptureSession *attachedSession) {
        QObject::connect(attachedSession, &consolation::capture::CaptureSession::logMessage, &app, [&](const QString &message) {
            out << "[capture] " << message << "\n";
            out.flush();
        });
        QObject::connect(
            attachedSession,
            &consolation::capture::CaptureSession::frameReady,
            &app,
            [&](const consolation::capture::FrameHandle &frame, qint64) {
            if (!frame || frame->isNull()) {
                return;
            }
            ++frames;
            out << "[frame] " << frames << " " << frame->width() << "x" << frame->height() << " format=" << frame->format() << "\n";
            out.flush();
            if (frames >= 5) {
                exitCode = 0;
                session->stop();
                app.quit();
            }
        });
        QObject::connect(attachedSession, &consolation::capture::CaptureSession::failed, &app, [&](const QString &message) {
            err << "[failed] " << message << "\n";
            err.flush();
            exitCode = 5;
            app.quit();
        });
    };

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

    const auto &format = device.formats[static_cast<size_t>(selectedFormatIndex)];
    out << "Starting device " << selectedDeviceIndex << " with format " << selectedFormatIndex << ": " << format.label << "\n";
    out.flush();
    session = manager.createSession(device.backend);
    if (!session) {
        err << "No session implementation for backend " << backendName(device.backend) << "\n";
        return 4;
    }
    attachSession(session.get());
    if (!session->start(device, format)) {
        return 7;
    }

    app.exec();
    session->stop();
    return exitCode;
}
