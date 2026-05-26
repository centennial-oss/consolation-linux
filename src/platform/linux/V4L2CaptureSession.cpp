#include "platform/linux/V4L2CaptureSession.h"

#include "capture/FourCc.h"

#include <QByteArray>
#include <QPointer>
#include <libyuv/convert.h>
#include <libyuv/convert_argb.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <numeric>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace consolation::platform::linux {

namespace {

constexpr qint64 telemetryWindowNs = 500'000'000;

qint64 monotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    const auto requestedDmaBufDisplay = dmaBufDisplayRequested_.load();
    stop();
    dmaBufDisplayRequested_.store(requestedDmaBufDisplay);
    dmaBufHandleFailureLogged_ = false;

    constexpr int maxAttempts = 3;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (attempt > 1) {
            emit logMessage(QStringLiteral("V4L2 retrying stream startup after reset/EBUSY (attempt %1 of %2)")
                                .arg(attempt)
                                .arg(maxAttempts));
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }

        if (!configureDevice(device, format) || !allocateBuffers() || !queueBuffers()) {
            const auto failureErrno = errno;
            cleanupBuffers();
            closeDevice();
            if (attempt < maxAttempts && (failureErrno == EBUSY || failureErrno == ENODEV || failureErrno == ENOENT)) {
                continue;
            }
            stop();
            return false;
        }

        emit logMessage(QStringLiteral("V4L2 starting stream with VIDIOC_STREAMON"));
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) == 0) {
            streaming_ = true;
            notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
            connect(notifier_, &QSocketNotifier::activated, this, [this]() { handleReadyRead(); });
            return true;
        }

        const auto streamErrno = errno;
        emit logMessage(QStringLiteral("V4L2 STREAMON failed on attempt %1: %2")
                            .arg(attempt)
                            .arg(QString::fromLocal8Bit(std::strerror(streamErrno))));
        cleanupBuffers();
        closeDevice();

        if (attempt >= maxAttempts || (streamErrno != EBUSY && streamErrno != ENODEV && streamErrno != ENOENT)) {
            emit failed(QStringLiteral("Could not start capture stream: %1").arg(QString::fromLocal8Bit(std::strerror(streamErrno))));
            return false;
        }
    }

    emit failed(QStringLiteral("Could not start capture stream after retrying device reset."));
    return false;
}

void V4L2CaptureSession::stop()
{
    dmaBufDisplayRequested_.store(false);
    const auto wasStreaming = streaming_;
    streaming_ = false;

    if (notifier_ != nullptr) {
        emit logMessage(QStringLiteral("V4L2 stop disabling socket notifier"));
        notifier_->setEnabled(false);
        delete notifier_;
        notifier_ = nullptr;
    }

    if (fd_ >= 0 && wasStreaming) {
        emit logMessage(QStringLiteral("V4L2 stopping stream with VIDIOC_STREAMOFF"));
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMOFF, &type) != 0) {
            emit logMessage(QStringLiteral("V4L2 STREAMOFF failed during stop: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
        }
    }

    cleanupBuffers();
    closeDevice();
    if (framePool_ != nullptr) {
        framePool_->reset();
        framePool_.reset();
    }
    emit logMessage(QStringLiteral("V4L2 stop complete"));
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

    v4l2_format current {};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &current) != 0) {
        emit failed(QStringLiteral("Could not read current capture format before configuring %1: %2").arg(
            device.devicePath,
            QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    emit logMessage(QStringLiteral("V4L2 request format %1x%2 %3 @ %4 fps")
                        .arg(format.width)
                        .arg(format.height)
                        .arg(format.pixelFormat)
                        .arg(format.framesPerSecond, 0, 'f', 2));
    const auto formatAlreadySelected =
        static_cast<int>(current.fmt.pix.width) == format.width &&
        static_cast<int>(current.fmt.pix.height) == format.height &&
        current.fmt.pix.pixelformat == requestedPixelFormat;
    if (formatAlreadySelected) {
        emit logMessage(QStringLiteral("V4L2 selected format already active; skipping VIDIOC_TRY_FMT and VIDIOC_S_FMT"));
    } else {
        v4l2_format requested = current;
        requested.fmt.pix.width = static_cast<__u32>(format.width);
        requested.fmt.pix.height = static_cast<__u32>(format.height);
        requested.fmt.pix.pixelformat = requestedPixelFormat;

        v4l2_format trial = requested;
        if (xioctl(fd_, VIDIOC_TRY_FMT, &trial) != 0) {
            emit failed(QStringLiteral("Could not validate capture format %1: %2").arg(
                format.label, QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }

        const auto trialMatches =
            static_cast<int>(trial.fmt.pix.width) == format.width &&
            static_cast<int>(trial.fmt.pix.height) == format.height &&
            trial.fmt.pix.pixelformat == requestedPixelFormat;
        if (!trialMatches) {
            emit failed(QStringLiteral("Capture device would negotiate %1x%2 %3 instead of requested %4; not applying format change.")
                            .arg(static_cast<int>(trial.fmt.pix.width))
                            .arg(static_cast<int>(trial.fmt.pix.height))
                            .arg(capture::fourCcToString(trial.fmt.pix.pixelformat), format.label));
            return false;
        }

        emit logMessage(QStringLiteral("V4L2 applying selected format with VIDIOC_S_FMT"));
        if (xioctl(fd_, VIDIOC_S_FMT, &requested) != 0) {
            emit failed(QStringLiteral("Could not set capture format %1: %2").arg(
                format.label, QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }

        emit logMessage(QStringLiteral("V4L2 format change applied; reopening device before buffer allocation"));
        closeDevice();
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        fd_ = ::open(device.devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            emit failed(QStringLiteral("Could not reopen %1 after format change: %2").arg(
                device.devicePath, QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }

        current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_G_FMT, &current) != 0) {
            emit failed(QStringLiteral("Could not read capture format after setting %1: %2").arg(
                format.label, QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }
    }

    v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_PARM, &parm) == 0) {
        const auto requestedTimePerFrame = fpsToTimePerFrame(format.framesPerSecond);
        const auto currentFps = timePerFrameToFps(parm.parm.capture.timeperframe);
        if (requestedTimePerFrame.numerator != 0 && std::abs(currentFps - format.framesPerSecond) > 0.5) {
            parm.parm.capture.timeperframe = requestedTimePerFrame;
            emit logMessage(QStringLiteral("V4L2 applying selected frame rate with VIDIOC_S_PARM"));
            if (xioctl(fd_, VIDIOC_S_PARM, &parm) != 0) {
                emit failed(QStringLiteral("Could not set capture frame rate %1 fps: %2").arg(
                    QString::number(format.framesPerSecond, 'f', 2),
                    QString::fromLocal8Bit(std::strerror(errno))));
                return false;
            }
        }
    }

    width_ = static_cast<int>(current.fmt.pix.width);
    height_ = static_cast<int>(current.fmt.pix.height);
    bytesPerLine_ = static_cast<int>(current.fmt.pix.bytesperline);
    pixelFormat_ = current.fmt.pix.pixelformat;
    configuredFps_ = format.framesPerSecond;

    emit logMessage(QStringLiteral("V4L2 current format %1x%2 %3 stride=%4")
                        .arg(width_)
                        .arg(height_)
                        .arg(capture::fourCcToString(pixelFormat_))
                        .arg(bytesPerLine_));

    if (width_ <= 0 || height_ <= 0) {
        emit failed(QStringLiteral("Capture device reported invalid format after negotiation: %1x%2").arg(width_).arg(height_));
        return false;
    }
    if (bytesPerLine_ <= 0) {
        if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
            bytesPerLine_ = width_ * 2;
        } else if (pixelFormat_ == V4L2_PIX_FMT_RGB24 || pixelFormat_ == V4L2_PIX_FMT_BGR24) {
            bytesPerLine_ = width_ * 3;
        } else {
            bytesPerLine_ = width_;
        }
    }
    if (width_ != format.width || height_ != format.height) {
        emit logMessage(QStringLiteral("V4L2 note: device negotiated %1x%2 (requested %3x%4)")
                            .arg(width_).arg(height_).arg(format.width).arg(format.height));
    }
    if (pixelFormat_ != requestedPixelFormat) {
        emit logMessage(QStringLiteral("V4L2 note: device negotiated pixel format %1 (requested %2)")
                            .arg(capture::fourCcToString(pixelFormat_), format.pixelFormat));
    }

    framePool_ = std::make_shared<capture::FrameBufferPool>();
    return true;
}

bool V4L2CaptureSession::allocateBuffers()
{
    emit logMessage(QStringLiteral("V4L2 requesting mmap capture buffers"));
    v4l2_requestbuffers request {};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
        emit failed(QStringLiteral("Could not allocate capture buffers: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    emit logMessage(QStringLiteral("V4L2 driver allocated %1 capture buffers").arg(request.count));

    buffers_.resize(request.count);
    auto dmaBufExportSupportedForAllBuffers = true;
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

        v4l2_exportbuffer exportRequest {};
        exportRequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exportRequest.index = index;
        exportRequest.plane = 0;
        if (xioctl(fd_, VIDIOC_EXPBUF, &exportRequest) == 0) {
            buffers_[index].dmaFd = exportRequest.fd;
        } else {
            dmaBufExportSupportedForAllBuffers = false;
        }
    }

    dmaBufExportSupported_ = dmaBufExportSupportedForAllBuffers;
    if (!dmaBufExportSupported_) {
        emit logMessage(QStringLiteral("V4L2 DMA-BUF export unavailable on one or more buffers; staying on mmap decode"));
    }

    if (dmaBufExportSupported_ && pixelFormatSupportsDmaBufDisplay(pixelFormat_)) {
        dmaBufDisplayEnabled_ = true;
        emit logMessage(
            QStringLiteral("V4L2 %1 DMA-BUF export enabled for zero-copy display")
                .arg(capture::fourCcToString(pixelFormat_)));
    }

    return true;
}

bool V4L2CaptureSession::queueBuffers()
{
    emit logMessage(QStringLiteral("V4L2 queueing capture buffers"));
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
    // Drain every frame queued by the driver this wakeup, but decode/emit only the newest.
    // Older buffers are requeued without decode to avoid redundant work and signal backlog.
    auto havePending = false;
    v4l2_buffer pending {};
    pending.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    pending.memory = V4L2_MEMORY_MMAP;

    while (true) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
            if (errno == EAGAIN) {
                break;
            }
            emit failed(QStringLiteral("Could not read capture frame: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            stop();
            return;
        }

        if (havePending) {
            if (fd_ < 0) {
                return; // stop() was called from within a frameReady handler; not an error
            }
            if (xioctl(fd_, VIDIOC_QBUF, &pending) != 0) {
                emit failed(QStringLiteral("Could not requeue capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
                stop();
                return;
            }
        }

        pending = buffer;
        havePending = true;

        if (fd_ < 0) {
            return; // stop() was called from within a frameReady handler; not an error
        }
    }

    if (!havePending) {
        return;
    }

    if (pending.index < buffers_.size()) {
        if (useDmaBufDisplayPath()) {
            if (auto dmaFrame = makeDmaBufFrameHandle(pending)) {
                recordDecodedFrame(static_cast<int>(pending.bytesused), 0, true);
                emit dmaFrameReady(std::move(dmaFrame));
                return;
            }
            if (!dmaBufHandleFailureLogged_) {
                dmaBufHandleFailureLogged_ = true;
                emit logMessage(QStringLiteral(
                    "V4L2 DMA-BUF handle creation failed; using mmap CPU decode for this session"));
            }
        }

        const auto decodeStartNs = monotonicNs();
        auto frame = decodeFrame(buffers_[pending.index].start, static_cast<int>(pending.bytesused));
        const auto decodeNs = monotonicNs() - decodeStartNs;
        if (frame && !frame->isNull()) {
            recordDecodedFrame(static_cast<int>(pending.bytesused), decodeNs, false);
            emit frameReady(std::move(frame));
        }
    }

    if (fd_ < 0) {
        return;
    }
    if (xioctl(fd_, VIDIOC_QBUF, &pending) != 0) {
        emit failed(QStringLiteral("Could not requeue capture buffer: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        stop();
    }
}

capture::FrameHandle V4L2CaptureSession::decodeFrame(const void *data, const int bytesUsed)
{
    if (data == nullptr || bytesUsed <= 0 || framePool_ == nullptr) {
        return {};
    }

    if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
        return decodeMjpeg(static_cast<const uchar *>(data), bytesUsed);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
        return decodeYuyv(static_cast<const uchar *>(data), bytesUsed);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_NV12) {
        return decodeNv12(static_cast<const uchar *>(data), bytesUsed);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_YUV420) {
        return decodeI420(static_cast<const uchar *>(data), bytesUsed, false);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_YVU420) {
        return decodeI420(static_cast<const uchar *>(data), bytesUsed, true);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_RGB24) {
        return decodeRgb24(static_cast<const uchar *>(data), bytesUsed, false, false);
    }

    if (pixelFormat_ == V4L2_PIX_FMT_BGR24) {
        // Some capture cards (e.g. blueAVS-BA12) emit BGR24 bottom-up (Windows DIB convention).
        // Flip vertically so the image appears right-side up.
        return decodeRgb24(static_cast<const uchar *>(data), bytesUsed, true, true);
    }

    return {};
}

QImage *V4L2CaptureSession::writableFramePixels(const capture::FrameHandle &frame)
{
    if (!frame) {
        return nullptr;
    }
    return const_cast<QImage *>(frame.get());
}

capture::FrameHandle V4L2CaptureSession::decodeMjpeg(const uchar *data, const int bytesUsed)
{
#ifdef HAVE_JPEG
    int jpegWidth = 0;
    int jpegHeight = 0;
    if (libyuv::MJPGSize(data, static_cast<size_t>(bytesUsed), &jpegWidth, &jpegHeight) == 0 && jpegWidth > 0 &&
        jpegHeight > 0) {
        auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
        auto *image = writableFramePixels(frame);
        if (image != nullptr) {
            const auto result = libyuv::MJPGToARGB(
                data,
                static_cast<size_t>(bytesUsed),
                image->bits(),
                image->bytesPerLine(),
                jpegWidth,
                jpegHeight,
                width_,
                height_);
            if (result == 0) {
                return frame;
            }
        }
    }
#endif
    return decodeMjpegQtFallback(data, bytesUsed);
}

capture::FrameHandle V4L2CaptureSession::decodeMjpegQtFallback(const uchar *data, const int bytesUsed)
{
    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    if (!frame) {
        return {};
    }
    const QImage decoded =
        QImage::fromData(data, bytesUsed).convertToFormat(QImage::Format_RGB32);
    if (decoded.isNull() || decoded.size() != frame->size() || decoded.bytesPerLine() != frame->bytesPerLine()) {
        return {};
    }
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }
    std::memcpy(image->bits(), decoded.constBits(), static_cast<size_t>(decoded.sizeInBytes()));
    return frame;
}

capture::FrameHandle V4L2CaptureSession::decodeYuyv(const uchar *data, const int bytesUsed)
{
    const auto stride = std::max(bytesPerLine_, width_ * 2);
    if (width_ <= 0 || height_ <= 0 || bytesUsed < stride * height_ || framePool_ == nullptr) {
        return {};
    }

    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }

    const auto result = libyuv::YUY2ToARGB(data, stride, image->bits(), image->bytesPerLine(), width_, height_);
    if (result != 0) {
        return {};
    }
    return frame;
}

capture::FrameHandle V4L2CaptureSession::decodeNv12(const uchar *data, const int bytesUsed)
{
    const auto stride = std::max(bytesPerLine_, width_);
    if (width_ <= 0 || height_ <= 0 || bytesUsed < stride * height_ * 3 / 2 || framePool_ == nullptr) {
        return {};
    }

    const auto *yPlane = data;
    const auto *uvPlane = data + stride * height_;
    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }
    const auto result = libyuv::NV12ToARGB(
        yPlane,
        stride,
        uvPlane,
        stride,
        image->bits(),
        image->bytesPerLine(),
        width_,
        height_);
    if (result != 0) {
        return {};
    }
    return frame;
}

capture::FrameHandle V4L2CaptureSession::decodeI420(const uchar *data, const int bytesUsed, const bool yvu)
{
    const auto yStride = std::max(bytesPerLine_, width_);
    const auto chromaStride = (yStride + 1) / 2;
    const auto chromaHeight = (height_ + 1) / 2;
    const auto yPlaneBytes = yStride * height_;
    const auto chromaPlaneBytes = chromaStride * chromaHeight;
    if (width_ <= 0 || height_ <= 0 || bytesUsed < yPlaneBytes + chromaPlaneBytes * 2 || framePool_ == nullptr) {
        return {};
    }

    const auto *yPlane = data;
    const auto *firstChromaPlane = yPlane + yPlaneBytes;
    const auto *secondChromaPlane = firstChromaPlane + chromaPlaneBytes;
    const auto *uPlane = yvu ? secondChromaPlane : firstChromaPlane;
    const auto *vPlane = yvu ? firstChromaPlane : secondChromaPlane;
    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }

    const auto result = libyuv::I420ToARGB(
        yPlane,
        yStride,
        uPlane,
        chromaStride,
        vPlane,
        chromaStride,
        image->bits(),
        image->bytesPerLine(),
        width_,
        height_);
    if (result != 0) {
        return {};
    }

    return frame;
}

capture::FrameHandle V4L2CaptureSession::decodeRgb24(
    const uchar *data,
    const int bytesUsed,
    const bool bgr,
    const bool flipVertical)
{
    const auto stride = std::max(bytesPerLine_, width_ * 3);
    if (width_ <= 0 || height_ <= 0 || bytesUsed < stride * height_ || framePool_ == nullptr) {
        return {};
    }

    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }

    const auto *source = flipVertical ? data + (height_ - 1) * stride : data;
    const auto sourceStride = flipVertical ? -stride : stride;
    const auto result = bgr
        ? libyuv::RGB24ToARGB(source, sourceStride, image->bits(), image->bytesPerLine(), width_, height_)
        : libyuv::RAWToARGB(source, sourceStride, image->bits(), image->bytesPerLine(), width_, height_);
    if (result != 0) {
        return {};
    }

    return frame;
}

void V4L2CaptureSession::recordDecodedFrame(const int bytesUsed, const qint64 decodeNs, const bool dmaBufPath)
{
    const auto nowNs = monotonicNs();
    if (telemetryWindowStartNs_ == 0) {
        telemetryWindowStartNs_ = nowNs;
    }

    ++telemetryFrameCount_;
    telemetryDecodeTotalNs_ += decodeNs;
    telemetryPayloadTotalBytes_ += bytesUsed;
    if (dmaBufPath) {
        ++telemetryDmaFrameCount_;
    } else {
        ++telemetryCpuFrameCount_;
    }

    const auto elapsedNs = nowNs - telemetryWindowStartNs_;
    if (elapsedNs < telemetryWindowNs) {
        return;
    }

    capture::VideoTelemetrySnapshot snapshot;
    snapshot.width = width_;
    snapshot.height = height_;
    snapshot.configuredFps = configuredFps_;
    snapshot.pixelFormatFourcc = pixelFormat_;
    snapshot.decodedFps = telemetryFrameCount_ * 1'000'000'000.0 / static_cast<double>(elapsedNs);
    snapshot.decodeAvgMs = telemetryFrameCount_ > 0
        ? static_cast<double>(telemetryDecodeTotalNs_) / static_cast<double>(telemetryFrameCount_) / 1'000'000.0
        : 0.0;
    snapshot.payloadAvgKb = telemetryFrameCount_ > 0
        ? static_cast<double>(telemetryPayloadTotalBytes_) / static_cast<double>(telemetryFrameCount_) / 1024.0
        : 0.0;
    snapshot.bufferCount = static_cast<int>(buffers_.size());
    snapshot.dmaFramesInWindow = telemetryDmaFrameCount_;
    snapshot.cpuFramesInWindow = telemetryCpuFrameCount_;
    snapshot.dmaCapturePathEnabled = useDmaBufDisplayPath();
    if (telemetryDmaFrameCount_ > 0 && telemetryCpuFrameCount_ == 0) {
        snapshot.frameMemory = capture::VideoFrameMemory::DmaBuf;
    } else if (telemetryCpuFrameCount_ > 0 && telemetryDmaFrameCount_ == 0) {
        snapshot.frameMemory = capture::VideoFrameMemory::Mmap;
    } else if (telemetryDmaFrameCount_ > 0 && telemetryCpuFrameCount_ > 0) {
        snapshot.frameMemory = capture::VideoFrameMemory::Mixed;
    }
    emit telemetryReady(snapshot);

    telemetryWindowStartNs_ = nowNs;
    telemetryFrameCount_ = 0;
    telemetryDecodeTotalNs_ = 0;
    telemetryPayloadTotalBytes_ = 0;
    telemetryDmaFrameCount_ = 0;
    telemetryCpuFrameCount_ = 0;
}

void V4L2CaptureSession::setDmaBufDisplayRequested(const bool requested)
{
    dmaBufDisplayRequested_.store(requested);
}

bool V4L2CaptureSession::pixelFormatSupportsDmaBufDisplay(const quint32 pixelFormat)
{
    return pixelFormat == V4L2_PIX_FMT_NV12 || pixelFormat == V4L2_PIX_FMT_RGB24 ||
        pixelFormat == V4L2_PIX_FMT_BGR24 || pixelFormat == V4L2_PIX_FMT_YUYV ||
        pixelFormat == v4l2_fourcc('Y', 'U', 'Y', '2');
}

bool V4L2CaptureSession::useDmaBufDisplayPath() const
{
    return dmaBufDisplayRequested_.load() && dmaBufDisplayEnabled_ && dmaBufExportSupported_ &&
        pixelFormatSupportsDmaBufDisplay(pixelFormat_) && fd_ >= 0 && streaming_;
}

capture::DmaBufFrameHandle V4L2CaptureSession::makeDmaBufFrameHandle(const v4l2_buffer &buffer)
{
    if (buffer.index >= buffers_.size()) {
        return {};
    }

    const auto &captureBuffer = buffers_[buffer.index];
    if (captureBuffer.dmaFd < 0) {
        return {};
    }

    auto *payload = new capture::DmaBufFrame();
    payload->bufferIndex = static_cast<int>(buffer.index);
    payload->dmaFd = captureBuffer.dmaFd;
    payload->width = width_;
    payload->height = height_;
    payload->bytesUsed = static_cast<int>(buffer.bytesused);

    if (pixelFormat_ == V4L2_PIX_FMT_NV12) {
        payload->layout = capture::DmaBufLayout::Nv12;
        payload->stride = std::max(bytesPerLine_, width_);
    } else if (pixelFormat_ == V4L2_PIX_FMT_RGB24) {
        payload->layout = capture::DmaBufLayout::Rgb888;
        payload->stride = std::max(bytesPerLine_, width_ * 3);
    } else if (pixelFormat_ == V4L2_PIX_FMT_BGR24) {
        payload->layout = capture::DmaBufLayout::Bgr888;
        payload->stride = std::max(bytesPerLine_, width_ * 3);
        payload->flipVertical = true;
    } else if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
        payload->layout = capture::DmaBufLayout::Yuyv422;
        payload->stride = std::max(bytesPerLine_, width_ * 2);
    } else {
        delete payload;
        return {};
    }

    const QPointer<V4L2CaptureSession> session(this);
    return capture::DmaBufFrameHandle(payload, [session](capture::DmaBufFrame *frame) {
        if (frame != nullptr) {
            const auto index = frame->bufferIndex;
            if (session != nullptr && session->streaming_ && session->fd_ >= 0) {
                QMetaObject::invokeMethod(
                    session,
                    "requeueCaptureBuffer",
                    Qt::QueuedConnection,
                    Q_ARG(int, index));
            }
            delete frame;
        }
    });
}

void V4L2CaptureSession::finishDmaFrameAsCpu(capture::DmaBufFrameHandle frame)
{
    if (!frame) {
        return;
    }

    if (dmaBufDisplayRequested_.load()) {
        emit logMessage(
            QStringLiteral("%1 DMA-BUF display falling back to CPU decode for one frame")
                .arg(capture::fourCcToString(pixelFormat_)));
    }

    const auto bufferIndex = frame->bufferIndex;
    const auto bytesUsed = frame->bytesUsed;

    if (!streaming_ || fd_ < 0 || bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) {
        frame.reset();
        return;
    }

    const auto decodeStartNs = monotonicNs();
    auto cpuFrame = decodeFrame(buffers_[static_cast<size_t>(bufferIndex)].start, bytesUsed);
    const auto decodeNs = monotonicNs() - decodeStartNs;
    frame.reset();

    if (cpuFrame && !cpuFrame->isNull()) {
        recordDecodedFrame(bytesUsed, decodeNs, false);
        emit frameReady(std::move(cpuFrame));
    }
}

void V4L2CaptureSession::requeueCaptureBuffer(const int bufferIndex)
{
    if (!streaming_ || fd_ < 0 || bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) {
        return;
    }

    v4l2_buffer buffer {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = static_cast<__u32>(bufferIndex);

    if (xioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
        emit logMessage(
            QStringLiteral("V4L2 DMA-BUF requeue failed for buffer %1: %2")
                .arg(bufferIndex)
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
}

void V4L2CaptureSession::cleanupBuffers()
{
    dmaBufDisplayEnabled_ = false;
    dmaBufExportSupported_ = false;

    for (auto &buffer : buffers_) {
        if (buffer.dmaFd >= 0) {
            ::close(buffer.dmaFd);
            buffer.dmaFd = -1;
        }
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
