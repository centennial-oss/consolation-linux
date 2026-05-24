#include "platform/linux/V4L2DeviceDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <set>
#include <sys/ioctl.h>
#include <unistd.h>

namespace consolation::platform::linux {

namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(const char *path)
        : fd_(::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC))
    {
    }

    ~FileDescriptor()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    [[nodiscard]] bool isValid() const { return fd_ >= 0; }
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_ = -1;
};

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

QString fourCcToString(const quint32 fourcc)
{
    std::array<char, 5> chars{
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
        '\0',
    };
    return QString::fromLatin1(chars.data()).trimmed();
}

QString formatFps(const double fps)
{
    if (std::abs(fps - std::round(fps)) < 0.01) {
        return QString::number(static_cast<int>(std::round(fps)));
    }
    return QString::number(fps, 'f', 2);
}

void addFormat(
    std::vector<capture::CaptureFormat> &formats,
    std::set<QString> &seen,
    const int width,
    const int height,
    const double fps,
    const QString &pixelFormat)
{
    if (width <= 0 || height <= 0 || fps <= 0.0 || pixelFormat.isEmpty()) {
        return;
    }

    const auto key = QStringLiteral("%1x%2@%3:%4")
                         .arg(width)
                         .arg(height)
                         .arg(formatFps(fps), pixelFormat);
    if (seen.contains(key)) {
        return;
    }
    seen.insert(key);

    capture::CaptureFormat format;
    format.width = width;
    format.height = height;
    format.framesPerSecond = fps;
    format.pixelFormat = pixelFormat;
    format.label = QStringLiteral("%1x%2 @ %3p · %4")
                       .arg(width)
                       .arg(height)
                       .arg(formatFps(fps), pixelFormat);
    formats.push_back(format);
}

void enumerateIntervals(
    const int fd,
    const quint32 pixelFormat,
    const int width,
    const int height,
    const QString &pixelFormatLabel,
    std::vector<capture::CaptureFormat> &formats,
    std::set<QString> &seen)
{
    v4l2_frmivalenum interval {};
    interval.pixel_format = pixelFormat;
    interval.width = static_cast<__u32>(width);
    interval.height = static_cast<__u32>(height);

    for (interval.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; ++interval.index) {
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (interval.discrete.numerator != 0) {
                const auto fps = static_cast<double>(interval.discrete.denominator) /
                    static_cast<double>(interval.discrete.numerator);
                addFormat(formats, seen, width, height, fps, pixelFormatLabel);
            }
            continue;
        }

        if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE || interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            if (interval.stepwise.min.numerator != 0) {
                const auto maxFps = static_cast<double>(interval.stepwise.min.denominator) /
                    static_cast<double>(interval.stepwise.min.numerator);
                addFormat(formats, seen, width, height, maxFps, pixelFormatLabel);
            }
            if (interval.stepwise.max.numerator != 0) {
                const auto minFps = static_cast<double>(interval.stepwise.max.denominator) /
                    static_cast<double>(interval.stepwise.max.numerator);
                addFormat(formats, seen, width, height, minFps, pixelFormatLabel);
            }
            break;
        }
    }
}

void enumerateSizes(
    const int fd,
    const quint32 pixelFormat,
    const QString &pixelFormatLabel,
    std::vector<capture::CaptureFormat> &formats,
    std::set<QString> &seen)
{
    v4l2_frmsizeenum size {};
    size.pixel_format = pixelFormat;

    for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            enumerateIntervals(
                fd,
                pixelFormat,
                static_cast<int>(size.discrete.width),
                static_cast<int>(size.discrete.height),
                pixelFormatLabel,
                formats,
                seen);
            continue;
        }

        if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE || size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            enumerateIntervals(
                fd,
                pixelFormat,
                static_cast<int>(size.stepwise.max_width),
                static_cast<int>(size.stepwise.max_height),
                pixelFormatLabel,
                formats,
                seen);
            enumerateIntervals(
                fd,
                pixelFormat,
                static_cast<int>(size.stepwise.min_width),
                static_cast<int>(size.stepwise.min_height),
                pixelFormatLabel,
                formats,
                seen);
            break;
        }
    }
}

std::vector<capture::CaptureFormat> enumerateFormats(const int fd)
{
    std::vector<capture::CaptureFormat> formats;
    std::set<QString> seen;

    v4l2_fmtdesc format {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
        enumerateSizes(fd, format.pixelformat, fourCcToString(format.pixelformat), formats, seen);
    }

    std::sort(formats.begin(), formats.end(), [](const auto &left, const auto &right) {
        if (left.width * left.height != right.width * right.height) {
            return left.width * left.height > right.width * right.height;
        }
        if (std::abs(left.framesPerSecond - right.framesPerSecond) > 0.01) {
            return left.framesPerSecond > right.framesPerSecond;
        }
        return left.pixelFormat < right.pixelFormat;
    });

    return formats;
}

bool isPrimaryVideoNode(const QString &devicePath)
{
    const auto nodeName = QFileInfo(devicePath).fileName();
    QFile indexFile(QStringLiteral("/sys/class/video4linux/%1/index").arg(nodeName));
    if (!indexFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return true;
    }

    bool ok = false;
    const auto index = QString::fromUtf8(indexFile.readAll()).trimmed().toInt(&ok);
    return !ok || index == 0;
}

} // namespace

std::vector<capture::CaptureDevice> V4L2DeviceDiscovery::enumerateDevices() const
{
    std::vector<capture::CaptureDevice> devices;
    const auto entries = QDir(QStringLiteral("/dev")).entryInfoList(
        QStringList{QStringLiteral("video*")},
        QDir::System | QDir::Readable | QDir::NoDotAndDotDot,
        QDir::Name);

    for (const auto &entry : entries) {
        const auto path = entry.absoluteFilePath();
        if (!isPrimaryVideoNode(path)) {
            continue;
        }

        const FileDescriptor fd(path.toLocal8Bit().constData());
        if (!fd.isValid()) {
            continue;
        }

        v4l2_capability capability {};
        if (xioctl(fd.get(), VIDIOC_QUERYCAP, &capability) != 0) {
            continue;
        }

        const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
            ? capability.device_caps
            : capability.capabilities;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
            continue;
        }
        if ((caps & V4L2_CAP_STREAMING) == 0) {
            continue;
        }

        const auto driver = QString::fromLocal8Bit(reinterpret_cast<const char *>(capability.driver)).trimmed();
        const auto busInfo = QString::fromLocal8Bit(reinterpret_cast<const char *>(capability.bus_info)).trimmed();
        if (driver != QStringLiteral("uvcvideo") && !busInfo.startsWith(QStringLiteral("usb-"))) {
            continue;
        }

        auto formats = enumerateFormats(fd.get());
        if (formats.empty()) {
            continue;
        }

        capture::CaptureDevice device;
        device.backend = capture::CaptureBackend::V4L2;
        device.devicePath = path;
        device.displayName = QString::fromLocal8Bit(reinterpret_cast<const char *>(capability.card)).trimmed();
        if (device.displayName.isEmpty()) {
            device.displayName = QFileInfo(path).fileName();
        }
        device.stableId = busInfo;
        if (device.stableId.isEmpty()) {
            device.stableId = path;
        }
        device.formats = std::move(formats);
        devices.push_back(std::move(device));
    }

    return devices;
}

} // namespace consolation::platform::linux
