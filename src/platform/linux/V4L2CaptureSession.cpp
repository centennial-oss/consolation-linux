#include "platform/linux/V4L2CaptureSession.h"

#include <QByteArray>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <numeric>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace consolation::platform::linux {

namespace {

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

quint32 stringToFourCc(const QString &value)
{
    const auto latin = value.toLatin1();
    if (latin.size() < 4) {
        return 0;
    }
    return v4l2_fourcc(latin[0], latin[1], latin[2], latin[3]);
}

QString fourCcToString(const quint32 fourcc)
{
    char chars[4] = {
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
    };
    return QString::fromLatin1(chars, 4).trimmed();
}

v4l2_fract fpsToTimePerFrame(const double fps)
{
    if (fps <= 0.0) {
        return {};
    }

    auto numerator = 1000U;
    auto denominator = static_cast<unsigned int>(std::round(fps * 1000.0));
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    return v4l2_fract{numerator, denominator};
}

double timePerFrameToFps(const v4l2_fract &timePerFrame)
{
    if (timePerFrame.numerator == 0) {
        return 0.0;
    }
    return static_cast<double>(timePerFrame.denominator) / static_cast<double>(timePerFrame.numerator);
}

int clampColor(const int value)
{
    return std::clamp(value, 0, 255);
}

QRgb yuvToRgb(const int y, const int u, const int v)
{
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    return qRgb(clampColor(r), clampColor(g), clampColor(b));
}

} // namespace

V4L2CaptureSession::V4L2CaptureSession(QObject *parent)
    : capture::CaptureSession(parent)
{
}

V4L2CaptureSession::~V4L2CaptureSession()
{
    stop();
}

bool V4L2CaptureSession::start(const capture::CaptureDevice &device, const capture::CaptureFormat &format)
{
    stop();

    if (!configureDevice(device, format) || !allocateBuffers() || !queueBuffers()) {
        stop();
        return false;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
        emit failed(QStringLiteral("Could not start capture stream: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        stop();
        return false;
    }

    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this]() { handleReadyRead(); });
    return true;
}

void V4L2CaptureSession::stop()
{
    if (notifier_ != nullptr) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }

    if (fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }

    cleanupBuffers();
    closeDevice();
}

bool V4L2CaptureSession::configureDevice(const capture::CaptureDevice &device, const capture::CaptureFormat &format)
{
    fd_ = ::open(device.devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        emit failed(QStringLiteral("Could not open %1: %2").arg(device.devicePath, QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    const auto requestedPixelFormat = stringToFourCc(format.pixelFormat);
    if (format.width <= 0 || format.height <= 0 || requestedPixelFormat == 0) {
        emit failed(QStringLiteral("Invalid V4L2 capture format: %1").arg(format.label));
        return false;
    }

    v4l2_format requested {};
    requested.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    requested.fmt.pix.width = static_cast<__u32>(format.width);
    requested.fmt.pix.height = static_cast<__u32>(format.height);
    requested.fmt.pix.pixelformat = requestedPixelFormat;
    requested.fmt.pix.field = V4L2_FIELD_NONE;

    emit logMessage(QStringLiteral("V4L2 request format %1x%2 %3 @ %4 fps")
                        .arg(format.width)
                        .arg(format.height)
                        .arg(format.pixelFormat)
                        .arg(format.framesPerSecond, 0, 'f', 2));

    if (xioctl(fd_, VIDIOC_S_FMT, &requested) != 0) {
        emit failed(QStringLiteral("Could not set capture format %1: %2").arg(
            format.label,
            QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    v4l2_streamparm requestedParm {};
    requestedParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    requestedParm.parm.capture.timeperframe = fpsToTimePerFrame(format.framesPerSecond);
    if (requestedParm.parm.capture.timeperframe.numerator != 0 &&
        xioctl(fd_, VIDIOC_S_PARM, &requestedParm) != 0) {
        emit failed(QStringLiteral("Could not set capture frame rate %1 fps: %2").arg(
            QString::number(format.framesPerSecond, 'f', 2),
            QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    v4l2_format current {};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &current) != 0) {
        emit failed(QStringLiteral("Could not read current capture format: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    v4l2_streamparm currentParm {};
    currentParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    const auto hasCurrentFps = xioctl(fd_, VIDIOC_G_PARM, &currentParm) == 0;
    const auto acceptedFps = hasCurrentFps ? timePerFrameToFps(currentParm.parm.capture.timeperframe) : 0.0;

    width_ = static_cast<int>(current.fmt.pix.width);
    height_ = static_cast<int>(current.fmt.pix.height);
    pixelFormat_ = current.fmt.pix.pixelformat;

    emit logMessage(QStringLiteral("V4L2 accepted format %1x%2 %3%4")
                        .arg(width_)
                        .arg(height_)
                        .arg(fourCcToString(pixelFormat_))
                        .arg(hasCurrentFps
                                 ? QStringLiteral(" @ %1 fps").arg(acceptedFps, 0, 'f', 2)
                                 : QString()));

    if (width_ != format.width || height_ != format.height || pixelFormat_ != requestedPixelFormat) {
        emit failed(QStringLiteral("Capture device accepted %1x%2 %3 instead of requested %4.").arg(
            QString::number(width_),
            QString::number(height_),
            fourCcToString(pixelFormat_),
            format.label));
        return false;
    }
    if (hasCurrentFps && format.framesPerSecond > 0.0 && std::abs(acceptedFps - format.framesPerSecond) > 0.5) {
        emit failed(QStringLiteral("Capture device accepted %1 fps instead of requested %2 fps.").arg(
            QString::number(acceptedFps, 'f', 2),
            QString::number(format.framesPerSecond, 'f', 2)));
        return false;
    }

    return true;
}

bool V4L2CaptureSession::allocateBuffers()
{
    v4l2_requestbuffers request {};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
        emit failed(QStringLiteral("Could not allocate capture buffers: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    buffers_.resize(request.count);
    for (auto index = 0U; index < request.count; ++index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
            emit failed(QStringLiteral("Could not query capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }

        buffers_[index].length = buffer.length;
        buffers_[index].start = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
        if (buffers_[index].start == MAP_FAILED) {
            buffers_[index].start = nullptr;
            emit failed(QStringLiteral("Could not map capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }
    }

    return true;
}

bool V4L2CaptureSession::queueBuffers()
{
    for (auto index = 0U; index < buffers_.size(); ++index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (xioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
            emit failed(QStringLiteral("Could not queue capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }
    }

    return true;
}

void V4L2CaptureSession::handleReadyRead()
{
    while (true) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
            if (errno == EAGAIN) {
                return;
            }
            emit failed(QStringLiteral("Could not read capture frame: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            stop();
            return;
        }

        if (buffer.index < buffers_.size()) {
            const auto image = decodeFrame(buffers_[buffer.index].start, static_cast<int>(buffer.bytesused));
            if (!image.isNull()) {
                emit frameReady(image);
            }
        }

        if (xioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
            emit failed(QStringLiteral("Could not requeue capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            stop();
            return;
        }
    }
}

QImage V4L2CaptureSession::decodeFrame(const void *data, const int bytesUsed) const
{
    if (data == nullptr || bytesUsed <= 0) {
        return {};
    }

    if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
        return QImage::fromData(static_cast<const uchar *>(data), bytesUsed).convertToFormat(QImage::Format_RGB32);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
        return decodeYuyv(static_cast<const uchar *>(data), bytesUsed);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_NV12) {
        return decodeNv12(static_cast<const uchar *>(data), bytesUsed);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_RGB24) {
        return QImage(static_cast<const uchar *>(data), width_, height_, QImage::Format_RGB888).copy();
    }

    if (pixelFormat_ == V4L2_PIX_FMT_BGR24) {
        return QImage(static_cast<const uchar *>(data), width_, height_, QImage::Format_BGR888).copy();
    }

    return {};
}

QImage V4L2CaptureSession::decodeYuyv(const uchar *data, const int bytesUsed) const
{
    if (width_ <= 0 || height_ <= 0 || bytesUsed < width_ * height_ * 2) {
        return {};
    }

    QImage image(width_, height_, QImage::Format_RGB32);
    for (int y = 0; y < height_; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const auto *source = data + y * width_ * 2;
        for (int x = 0; x < width_; x += 2) {
            const int y0 = source[0];
            const int u = source[1];
            const int y1 = source[2];
            const int v = source[3];
            line[x] = yuvToRgb(y0, u, v);
            if (x + 1 < width_) {
                line[x + 1] = yuvToRgb(y1, u, v);
            }
            source += 4;
        }
    }
    return image;
}

QImage V4L2CaptureSession::decodeNv12(const uchar *data, const int bytesUsed) const
{
    if (width_ <= 0 || height_ <= 0 || bytesUsed < width_ * height_ * 3 / 2) {
        return {};
    }

    const auto *yPlane = data;
    const auto *uvPlane = data + width_ * height_;
    QImage image(width_, height_, QImage::Format_RGB32);

    for (int y = 0; y < height_; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const auto *yLine = yPlane + y * width_;
        const auto *uvLine = uvPlane + (y / 2) * width_;
        for (int x = 0; x < width_; ++x) {
            const int uvIndex = (x / 2) * 2;
            line[x] = yuvToRgb(yLine[x], uvLine[uvIndex], uvLine[uvIndex + 1]);
        }
    }

    return image;
}

void V4L2CaptureSession::cleanupBuffers()
{
    for (auto &buffer : buffers_) {
        if (buffer.start != nullptr) {
            ::munmap(buffer.start, buffer.length);
            buffer.start = nullptr;
            buffer.length = 0;
        }
    }
    buffers_.clear();
}

void V4L2CaptureSession::closeDevice()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace consolation::platform::linux
