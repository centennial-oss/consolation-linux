#include "platform/linux/V4L2CaptureSession.h"

#include "capture/FourCc.h"
#include "capture/MonotonicClock.h"
#include "platform/linux/DmaBufSync.h"
#include "platform/linux/MjpegPerfLog.h"

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

#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#endif
#include <numeric>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace consolation::platform::linux {

namespace {

constexpr qint64 telemetryWindowNs = 500'000'000;

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
    , requeuePending_(std::make_shared<std::atomic<uint64_t>>(0))
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
    dmaBufWarmupSkipLogged_ = false;
    requeuePending_ = std::make_shared<std::atomic<uint64_t>>(0);

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
            startMjpegDecodeWorker();
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

    // Join the decode worker before tearing down the VA decoder / buffers it reads.
    stopMjpegDecodeWorker();

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
    dmaFramePool_.reset();
    vaapiMjpegDmaPool_.reset();
    vaapiMjpegDmaEnabled_ = false;
    mjpegDecoder_.reset();
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
        } else if (pixelFormat_ == V4L2_PIX_FMT_P010) {
            bytesPerLine_ = width_ * 2;
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
    vaapiMjpegDmaPool_.reset();
    vaapiMjpegDmaEnabled_ = false;
    if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
        mjpegDecoder_.configure(width_, height_);
        const auto dmaDisplayRequested = dmaBufDisplayRequested_.load();
        const auto decoderSupportsDma = mjpegDecoder_.supportsDmaDecode();
        vaapiMjpegDmaEnabled_ = dmaDisplayRequested && decoderSupportsDma;
        if (vaapiMjpegDmaEnabled_) {
            constexpr std::size_t kVaapiMjpegDmaSlots = 8;
            vaapiMjpegDmaPool_ = std::make_shared<std::vector<capture::DmaBufFrame>>(kVaapiMjpegDmaSlots);
            emit logMessage(QStringLiteral("VA-API MJPEG DMA-BUF display enabled (zero-copy NV12)"));
        } else {
            emit logMessage(QStringLiteral(
                "VA-API MJPEG DMA-BUF display disabled (dmaDisplayRequested=%1, decoderSupportsDmaExport=%2); "
                "using hardware decode to ARGB CPU-upload path")
                                .arg(dmaDisplayRequested ? QStringLiteral("yes") : QStringLiteral("no"))
                                .arg(decoderSupportsDma ? QStringLiteral("yes") : QStringLiteral("no")));
        }
    } else {
        mjpegDecoder_.reset();
    }
    return true;
}

bool V4L2CaptureSession::allocateBuffers()
{
    emit logMessage(QStringLiteral("V4L2 requesting mmap capture buffers"));
    v4l2_requestbuffers request {};
    request.count = captureBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
        emit failed(QStringLiteral("Could not allocate capture buffers: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    emit logMessage(QStringLiteral("V4L2 driver allocated %1 capture buffers").arg(request.count));

    buffers_.resize(request.count);
    dmaFramePool_ = std::make_shared<std::vector<capture::DmaBufFrame>>(request.count);
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
    const auto traceEnabled = frame_trace::enabled();
    const qint64 wakeNs = capture::monotonicClockNs();

    // Return any buffers that were released by the UI thread since the last wakeup.
    drainRequeuePending();

    // Drain every frame queued by the driver this wakeup, but decode/emit only the newest.
    // Older buffers are requeued without decode to avoid redundant work and signal backlog.
    auto havePending = false;
    v4l2_buffer pending {};
    pending.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    pending.memory = V4L2_MEMORY_MMAP;

    int drainCount = 0;
    qint64 newestDqNs = 0;

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

        if (buffer.index < buffers_.size()) {
            const auto dqNs = capture::monotonicClockNs();
            const bool isMono = (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) != 0;
            buffers_[buffer.index].capturedAtNs = isMono
                ? static_cast<qint64>(buffer.timestamp.tv_sec) * 1'000'000'000LL
                  + static_cast<qint64>(buffer.timestamp.tv_usec) * 1'000LL
                : dqNs;
            buffers_[buffer.index].presentationBaseNs = dqNs;
            buffers_[buffer.index].dqMonotonicNs = dqNs;
            buffers_[buffer.index].wakeAtNs = wakeNs;
            if (traceEnabled) {
                newestDqNs = dqNs;
                ++drainCount;
            }
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
        if (useVaapiMjpegDmaPath()) {
            const auto bytesUsed = static_cast<int>(pending.bytesused);
            const auto capturedAtNs = buffers_[pending.index].capturedAtNs;
            // Hand the newest compressed payload to the decode worker and requeue the V4L2 buffer
            // immediately. VA submit/sync/export no longer runs on the capture/notifier thread (items
            // 4-5); copying the payload lets us QBUF now without waiting for libva to consume the source
            // buffer (item 2, MJPEG path), keeping the capture loop at drain=1.
            const auto slotsAvailable = mjpegDecoder_.dmaSlotsAvailable();
            quint64 traceSeq = 0;
            if (traceEnabled) {
                const auto handoffNs = capture::monotonicClockNs();
                traceSeq = frame_trace::nextSeq();
                frame_trace::CaptureSpan span;
                span.seq = traceSeq;
                span.fourcc = pixelFormat_;
                span.captureBufferIndex = static_cast<int>(pending.index);
                span.capturedAtNs = capturedAtNs;
                span.captureAgeMs = static_cast<double>(wakeNs - capturedAtNs) / 1'000'000.0;
                span.dqAgeMs = static_cast<double>(newestDqNs - capturedAtNs) / 1'000'000.0;
                span.drainCount = drainCount;
                span.dqToEmitMs = static_cast<double>(handoffNs - newestDqNs) / 1'000'000.0;
                span.bytesUsed = bytesUsed;
                // dq-to-handoff now; decode latency moves to the worker and the present span.
                span.path = slotsAvailable ? "vaapi-mjpeg-dma-queued" : "vaapi-mjpeg-dma-slot-starved";
                frame_trace::recordCapture(span);
            }
            // Drop the frame when no VA surface slot is free (transient backlog) — the worker always
            // keeps the newest job, so capture stays responsive rather than blocking on decode.
            if (slotsAvailable) {
                queueMjpegForDecode(
                    static_cast<const uchar *>(buffers_[pending.index].start),
                    bytesUsed,
                    capturedAtNs,
                    buffers_[pending.index].wakeAtNs,
                    traceSeq,
                    static_cast<int>(pending.index));
            }
            if (fd_ >= 0 && xioctl(fd_, VIDIOC_QBUF, &pending) != 0) {
                emit failed(QStringLiteral("Could not requeue capture buffer: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
                stop();
            }
            return;
        }

        if (useDmaBufDisplayPath()) {
            const auto bytesUsed = static_cast<int>(pending.bytesused);
            if (frameReadyForDmaDisplay(bytesUsed, pending.flags)) {
                if (auto dmaFrame = makeDmaBufFrameHandle(pending)) {
                    if (traceEnabled) {
                        const auto seq = frame_trace::nextSeq();
                        const auto emitNs = capture::monotonicClockNs();
                const auto capturedAtNs = buffers_[pending.index].capturedAtNs;
                        dmaFrame->seq = seq;
                        dmaFrame->emittedAtNs = emitNs;
                        buffers_[pending.index].traceSeq = seq;
                        outstandingCaptureBuffers_.fetch_add(1, std::memory_order_relaxed);
                        frame_trace::CaptureSpan span;
                        span.seq = seq;
                        span.fourcc = pixelFormat_;
                        span.captureBufferIndex = static_cast<int>(pending.index);
                        span.capturedAtNs = capturedAtNs;
                        span.captureAgeMs = static_cast<double>(wakeNs - capturedAtNs) / 1'000'000.0;
                        span.dqAgeMs = static_cast<double>(newestDqNs - capturedAtNs) / 1'000'000.0;
                        span.drainCount = drainCount;
                        span.dqToEmitMs = static_cast<double>(emitNs - newestDqNs) / 1'000'000.0;
                        span.bytesUsed = bytesUsed;
                        span.path = "v4l2-dma";
                        frame_trace::recordCapture(span);
                    }
                    dmaFrame->publishSeq = ++dmaPublishSeq_;
                    latestDmaSeq.store(dmaPublishSeq_, std::memory_order_release);
                    emit dmaFrameReady(std::move(dmaFrame));
                    return;
                }
            } else if (!dmaBufWarmupSkipLogged_) {
                dmaBufWarmupSkipLogged_ = true;
                emit logMessage(QStringLiteral(
                    "V4L2 skipping DMA-BUF display for warmup/error frame (bytesused=%1, flags=0x%2)")
                                    .arg(bytesUsed)
                                    .arg(static_cast<uint>(pending.flags), 0, 16));
            }
            if (!dmaBufHandleFailureLogged_) {
                dmaBufHandleFailureLogged_ = true;
                emit logMessage(QStringLiteral(
                    "V4L2 DMA-BUF handle creation failed; using mmap CPU decode for this session"));
            }
        }

        const auto capturedAtNs = buffers_[pending.index].capturedAtNs;
        const auto decodeStartNs = capture::monotonicClockNs();
        auto frame = decodeFrame(buffers_[pending.index].start, static_cast<int>(pending.bytesused));
        const auto decodeNs = capture::monotonicClockNs() - decodeStartNs;
        if (frame && !frame->isNull()) {
            recordDecodedFrame(static_cast<int>(pending.bytesused), decodeNs, false);
            if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
                const char *detail = mjpegDecoder_.hardwareDecodeActive() ? "vaapi-cpu-argb" : "software";
                mjpeg_perf::recordSessionDecode(static_cast<double>(decodeNs) / 1'000'000.0, false, detail);
            }
            if (traceEnabled) {
                const auto emitNs = capture::monotonicClockNs();
                // "cpu" here means the non-zero-copy present path (decoded into an ARGB QImage
                // uploaded by the renderer), not necessarily a software decode. For MJPEG, report
                // the actual decode backend so the trace agrees with the HWJD overlay.
                const char *tracePath = "cpu";
                if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
                    switch (telemetryLastMjpegPath_) {
                    case capture::MjpegDecodePath::Vaapi:   tracePath = "mjpeg-vaapi-argb"; break;
                    case capture::MjpegDecodePath::V4l2M2m: tracePath = "mjpeg-v4l2m2m-argb"; break;
                    case capture::MjpegDecodePath::Libyuv:  tracePath = "mjpeg-libyuv"; break;
                    case capture::MjpegDecodePath::Qt:      tracePath = "mjpeg-qt"; break;
                    default:                                tracePath = "mjpeg-software"; break;
                    }
                }
                frame_trace::CaptureSpan span;
                span.seq = frame_trace::nextSeq();
                span.fourcc = pixelFormat_;
                span.captureBufferIndex = static_cast<int>(pending.index);
                span.capturedAtNs = capturedAtNs;
                span.captureAgeMs = static_cast<double>(wakeNs - capturedAtNs) / 1'000'000.0;
                span.dqAgeMs = static_cast<double>(newestDqNs - capturedAtNs) / 1'000'000.0;
                span.drainCount = drainCount;
                span.dqToEmitMs = static_cast<double>(emitNs - newestDqNs) / 1'000'000.0;
                span.bytesUsed = static_cast<int>(pending.bytesused);
                span.path = tracePath;
                frame_trace::recordCapture(span);
            }
            emit frameReady(std::move(frame), capturedAtNs, buffers_[pending.index].wakeAtNs);
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

    if (pixelFormat_ == V4L2_PIX_FMT_P010) {
        return decodeP010(static_cast<const uchar *>(data), bytesUsed);
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
    if (data == nullptr || bytesUsed <= 0 || framePool_ == nullptr || width_ <= 0 || height_ <= 0) {
        return {};
    }

    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }

    if (mjpegDecoder_.decode(
            data,
            bytesUsed,
            width_,
            height_,
            image->bits(),
            image->bytesPerLine())) {
        telemetryLastMjpegPath_ = mjpegDecoder_.lastPath();
        return frame;
    }

    return {};
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

capture::FrameHandle V4L2CaptureSession::decodeP010(const uchar *data, const int bytesUsed)
{
    const auto byteStride = std::max(bytesPerLine_, width_ * 2);
    const auto sampleStride = byteStride / 2;
    if (width_ <= 0 || height_ <= 0 || bytesUsed < byteStride * height_ * 3 / 2 || framePool_ == nullptr) {
        return {};
    }

    const auto *yPlane = reinterpret_cast<const uint16_t *>(data);
    const auto *uvPlane = reinterpret_cast<const uint16_t *>(data + static_cast<size_t>(byteStride) * static_cast<size_t>(height_));
    auto frame = framePool_->acquireForDecode(width_, height_, QImage::Format_RGB32);
    auto *image = writableFramePixels(frame);
    if (image == nullptr) {
        return {};
    }

    const auto result = libyuv::P010ToARGBMatrix(
        yPlane,
        sampleStride,
        uvPlane,
        sampleStride,
        image->bits(),
        image->bytesPerLine(),
        &libyuv::kYuvH709Constants,
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

bool V4L2CaptureSession::frameReadyForDmaDisplay(const int bytesUsed, const __u32 bufferFlags) const
{
    if (bytesUsed <= 0 || width_ <= 0 || height_ <= 0) {
        return false;
    }

    if ((bufferFlags & V4L2_BUF_FLAG_ERROR) != 0) {
        return false;
    }

    const auto yStride = std::max(bytesPerLine_, width_);
    if (pixelFormat_ == V4L2_PIX_FMT_NV12) {
        return bytesUsed >= yStride * height_ * 3 / 2;
    }
    if (pixelFormat_ == V4L2_PIX_FMT_P010) {
        const auto byteStride = std::max(bytesPerLine_, width_ * 2);
        return bytesUsed >= byteStride * height_ * 3 / 2;
    }
    if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
        return bytesUsed >= std::max(bytesPerLine_, width_ * 2) * height_;
    }
    if (pixelFormat_ == V4L2_PIX_FMT_YUV420 || pixelFormat_ == V4L2_PIX_FMT_YVU420) {
        const auto chromaStride = (yStride + 1) / 2;
        const auto chromaHeight = (height_ + 1) / 2;
        return bytesUsed >= yStride * height_ + chromaStride * chromaHeight * 2;
    }
    if (pixelFormat_ == V4L2_PIX_FMT_RGB24 || pixelFormat_ == V4L2_PIX_FMT_BGR24) {
        return bytesUsed >= std::max(bytesPerLine_, width_ * 3) * height_;
    }

    return false;
}

void V4L2CaptureSession::recordDecodedFrame(const int bytesUsed, const qint64 decodeNs, const bool dmaBufPath)
{
    const auto nowNs = capture::monotonicClockNs();
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
        if (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) {
            if (capture::mjpegDecodePathIsHardware(telemetryLastMjpegPath_)) {
                ++telemetryMjpegHwFrameCount_;
            }
        }
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
    snapshot.mjpegDecodePath = telemetryLastMjpegPath_;
    snapshot.mjpegHwDecodeActive =
        telemetryMjpegHwFrameCount_ > 0 &&
        (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG);
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
    telemetryMjpegHwFrameCount_ = 0;
}

void V4L2CaptureSession::setDmaBufDisplayRequested(const bool requested)
{
    dmaBufDisplayRequested_.store(requested);
}

bool V4L2CaptureSession::pixelFormatSupportsDmaBufDisplay(const quint32 pixelFormat)
{
    return pixelFormat == V4L2_PIX_FMT_NV12 || pixelFormat == V4L2_PIX_FMT_P010 ||
        pixelFormat == V4L2_PIX_FMT_RGB24 || pixelFormat == V4L2_PIX_FMT_BGR24 ||
        pixelFormat == V4L2_PIX_FMT_YUYV || pixelFormat == v4l2_fourcc('Y', 'U', 'Y', '2') ||
        pixelFormat == V4L2_PIX_FMT_YUV420 || pixelFormat == V4L2_PIX_FMT_YVU420;
}

bool V4L2CaptureSession::useDmaBufDisplayPath() const
{
    return !useVaapiMjpegDmaPath() && dmaBufDisplayRequested_.load() && dmaBufDisplayEnabled_ &&
        dmaBufExportSupported_ && pixelFormatSupportsDmaBufDisplay(pixelFormat_) && fd_ >= 0 && streaming_;
}

bool V4L2CaptureSession::useVaapiMjpegDmaPath() const
{
    return vaapiMjpegDmaEnabled_ && !vaapiMjpegDmaRuntimeFailed_.load(std::memory_order_acquire) &&
        dmaBufDisplayRequested_.load() && streaming_ && fd_ >= 0 &&
        (pixelFormat_ == V4L2_PIX_FMT_MJPEG || pixelFormat_ == V4L2_PIX_FMT_JPEG) && vaapiMjpegDmaPool_ != nullptr;
}

void V4L2CaptureSession::releaseVaapiMjpegDmaResources(const int vaapiSlotIndex)
{
    if (frame_trace::enabled()) {
        const auto releaseNs = capture::monotonicClockNs();
        frame_trace::BufferLifetimeSpan span;
        span.event = "va-slot-release";
        span.vaSurfaceSlot = vaapiSlotIndex;
        if (vaapiSlotIndex >= 0 && static_cast<std::size_t>(vaapiSlotIndex) < kTraceVaapiSlots) {
            const auto slotIdx = static_cast<std::size_t>(vaapiSlotIndex);
            span.seq = vaapiSlotSeq_[slotIdx].load(std::memory_order_relaxed);
            const auto handoutNs = vaapiSlotHandoutNs_[slotIdx].load(std::memory_order_relaxed);
            if (handoutNs != 0) {
                span.vaSlotHoldMs = static_cast<double>(releaseNs - handoutNs) / 1'000'000.0;
            }
        }
        span.outstandingVaSlots = outstandingVaapiSlots_.fetch_sub(1, std::memory_order_relaxed) - 1;
        frame_trace::recordBufferLifetime(span);
    }

    mjpegDecoder_.releaseDmaSlot(vaapiSlotIndex);
}

void V4L2CaptureSession::noteRequeueRequested(const int bufferIndex)
{
    if (!frame_trace::enabled() || bufferIndex < 0 ||
        static_cast<std::size_t>(bufferIndex) >= kTraceCaptureBufferSlots) {
        return;
    }
    requeueRequestedNs_[static_cast<std::size_t>(bufferIndex)].store(
        capture::monotonicClockNs(), std::memory_order_relaxed);
}

capture::DmaBufFrameHandle V4L2CaptureSession::makeVaapiMjpegDmaFrameHandle(
    const mjpeg_backend::VaapiDmaFrame &decoded,
    const MjpegDecodeJob &job)
{
    // Called on the decode worker thread. The source V4L2 buffer was already requeued by the capture
    // thread (payload was copied into job.payload), so this handle only owns the VA surface slot +
    // exported plane FDs — its deleter does NOT requeue a V4L2 buffer.
    if (decoded.dmaFd < 0 || decoded.slotIndex < 0 || decoded.width <= 0 || decoded.height <= 0 ||
        decoded.stride <= 0 || !vaapiMjpegDmaPool_ ||
        decoded.slotIndex >= static_cast<int>(vaapiMjpegDmaPool_->size())) {
        return {};
    }

    capture::DmaBufFrame &slot = (*vaapiMjpegDmaPool_)[static_cast<size_t>(decoded.slotIndex)];
    slot.bufferIndex = decoded.slotIndex;
    slot.captureBufferIndex = job.captureBufferIndex;
    slot.dmaFd = decoded.dmaFd;
    slot.planeFds = decoded.planeFds;
    slot.planeOffsets = decoded.planeOffsets;
    slot.planeStrides = decoded.planeStrides;
    slot.planeFourccs = decoded.planeFourccs;
    slot.planeModifiers = decoded.planeModifiers;
    slot.width = decoded.width;
    slot.height = decoded.height;
    slot.stride = decoded.stride;
    slot.bytesUsed = job.bytesUsed;
    slot.capturedAtNs = job.capturedAtNs;
    slot.wakeAtNs = job.wakeAtNs;
    slot.origin = capture::DmaBufOrigin::VaapiMjpeg;
    slot.layout = capture::DmaBufLayout::Nv12;
    slot.flipVertical = false;

    if (frame_trace::enabled()) {
        const auto slotIdx = static_cast<std::size_t>(decoded.slotIndex);
        if (slotIdx < kTraceVaapiSlots) {
            vaapiSlotHandoutNs_[slotIdx].store(capture::monotonicClockNs(), std::memory_order_relaxed);
            vaapiSlotSeq_[slotIdx].store(job.traceSeq, std::memory_order_relaxed);
        }
        outstandingVaapiSlots_.fetch_add(1, std::memory_order_relaxed);
    }

    auto pool = vaapiMjpegDmaPool_;
    const auto vaapiSlotIndex = decoded.slotIndex;
    const QPointer<V4L2CaptureSession> session(this);
    return capture::DmaBufFrameHandle(
        &slot,
        [pool = std::move(pool), vaapiSlotIndex, session](capture::DmaBufFrame *) {
            if (session) {
                session->releaseVaapiMjpegDmaResources(vaapiSlotIndex);
            }
        });
}

void V4L2CaptureSession::startMjpegDecodeWorker()
{
    if (mjpegWorker_.joinable()) {
        return; // already running
    }
    if (!vaapiMjpegDmaEnabled_ || vaapiMjpegDmaPool_ == nullptr ||
        (pixelFormat_ != V4L2_PIX_FMT_MJPEG && pixelFormat_ != V4L2_PIX_FMT_JPEG)) {
        return; // VA-API MJPEG zero-copy path not in use; no worker needed
    }
    {
        std::lock_guard<std::mutex> lock(mjpegMutex_);
        mjpegWorkerStop_ = false;
        mjpegJobValid_ = false;
    }
    vaapiMjpegDmaRuntimeFailed_.store(false, std::memory_order_release);
    mjpegWorker_ = std::thread([this]() { mjpegDecodeLoop(); });
}

void V4L2CaptureSession::stopMjpegDecodeWorker()
{
    if (!mjpegWorker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mjpegMutex_);
        mjpegWorkerStop_ = true;
        mjpegJobValid_ = false;
    }
    mjpegCv_.notify_all();
    mjpegWorker_.join();
}

void V4L2CaptureSession::queueMjpegForDecode(
    const uchar *payload,
    const int bytesUsed,
    const qint64 capturedAtNs,
    const qint64 wakeAtNs,
    const quint64 traceSeq,
    const int captureBufferIndex)
{
    if (payload == nullptr || bytesUsed <= 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mjpegMutex_);
        // Depth-one: overwrite any prior undecoded job (newest wins). assign() reuses the buffer's
        // capacity, so no per-frame allocation once it has grown to the payload size.
        mjpegJob_.payload.assign(payload, payload + bytesUsed);
        mjpegJob_.bytesUsed = bytesUsed;
        mjpegJob_.capturedAtNs = capturedAtNs;
        mjpegJob_.wakeAtNs = wakeAtNs;
        mjpegJob_.traceSeq = traceSeq;
        mjpegJob_.captureBufferIndex = captureBufferIndex;
        mjpegJobValid_ = true;
    }
    mjpegCv_.notify_one();
}

void V4L2CaptureSession::mjpegDecodeLoop()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mjpegMutex_);
            mjpegCv_.wait(lock, [this]() { return mjpegJobValid_ || mjpegWorkerStop_; });
            if (mjpegWorkerStop_) {
                return;
            }
            // Take the newest job; swap (O(1)) so the worker reuses its payload buffer capacity.
            std::swap(mjpegWorkerJob_, mjpegJob_);
            mjpegJobValid_ = false;
        }
        if (vaapiMjpegDmaRuntimeFailed_.load(std::memory_order_acquire)) {
            continue; // path disabled at runtime; the capture thread now decodes on the CPU path
        }
        decodeMjpegJob(mjpegWorkerJob_);
    }
}

void V4L2CaptureSession::decodeMjpegJob(MjpegDecodeJob &job)
{
    if (!mjpegDecoder_.dmaSlotsAvailable()) {
        return; // no VA surface free; drop (newest-wins keeps the freshest frame)
    }

    const auto decodeStartNs = capture::monotonicClockNs();
    mjpeg_backend::VaapiDmaFrame decoded {};
    if (!mjpegDecoder_.decodeDma(job.payload.data(), job.bytesUsed, decoded)) {
        vaapiMjpegDmaRuntimeFailed_.store(true, std::memory_order_release);
        emit logMessage(QStringLiteral(
            "VA-API MJPEG DMA-BUF decode/export failed; falling back to hardware decode to ARGB CPU-upload path"));
        return;
    }
    const auto decodeMs = static_cast<double>(capture::monotonicClockNs() - decodeStartNs) / 1'000'000.0;

    auto dmaFrame = makeVaapiMjpegDmaFrameHandle(decoded, job);
    if (!dmaFrame) {
        for (const auto fd : decoded.planeFds) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        mjpegDecoder_.releaseDmaSlot(decoded.slotIndex);
        vaapiMjpegDmaRuntimeFailed_.store(true, std::memory_order_release);
        emit logMessage(QStringLiteral(
            "VA-API MJPEG DMA-BUF handle creation failed; falling back to hardware decode to ARGB CPU-upload path"));
        return;
    }

    if (frame_trace::enabled()) {
        dmaFrame->seq = job.traceSeq;
        dmaFrame->emittedAtNs = capture::monotonicClockNs();
    }
    dmaFrame->publishSeq = ++dmaPublishSeq_;
    latestDmaSeq.store(dmaPublishSeq_, std::memory_order_release);

    // Apply decode telemetry on the capture thread so it does not race recordDmaFramePresented().
    QMetaObject::invokeMethod(
        this, [this, decodeMs]() { recordVaapiDmaDecodeTelemetry(decodeMs); }, Qt::QueuedConnection);

    emit dmaFrameReady(std::move(dmaFrame));
}

void V4L2CaptureSession::recordVaapiDmaDecodeTelemetry(const double decodeMs)
{
    telemetryLastMjpegPath_ = capture::MjpegDecodePath::Vaapi;
    mjpeg_perf::recordSessionDecode(decodeMs, true, "vaapi-dma-buf");
}

capture::DmaBufFrameHandle V4L2CaptureSession::makeDmaBufFrameHandle(const v4l2_buffer &buffer)
{
    if (buffer.index >= buffers_.size() || !dmaFramePool_) {
        return {};
    }

    const auto &captureBuffer = buffers_[buffer.index];
    if (captureBuffer.dmaFd < 0) {
        return {};
    }

    // Use the pre-allocated pool slot for this V4L2 buffer index — no heap allocation per frame.
    capture::DmaBufFrame &slot = (*dmaFramePool_)[buffer.index];
    slot.bufferIndex = static_cast<int>(buffer.index);
    slot.captureBufferIndex = static_cast<int>(buffer.index);
    slot.origin = capture::DmaBufOrigin::V4L2Capture;
    slot.dmaFd = captureBuffer.dmaFd;
    slot.planeFds = { captureBuffer.dmaFd, -1 };
    slot.planeOffsets = { 0, 0 };
    slot.planeStrides = { 0, 0 };
    slot.planeFourccs = { 0, 0 };
    slot.planeModifiers = { capture::DmaBufFrame::invalidModifier, capture::DmaBufFrame::invalidModifier };
    slot.width = width_;
    slot.height = height_;
    slot.bytesUsed = static_cast<int>(buffer.bytesused);
    slot.capturedAtNs = captureBuffer.capturedAtNs;
    slot.wakeAtNs = captureBuffer.wakeAtNs;
    slot.flipVertical = false;

    if (pixelFormat_ == V4L2_PIX_FMT_NV12) {
        slot.layout = capture::DmaBufLayout::Nv12;
        slot.stride = std::max(bytesPerLine_, width_);
        slot.planeFds[1] = captureBuffer.dmaFd;
        slot.planeOffsets[1] = slot.stride * height_;
        slot.planeStrides[0] = slot.stride;
        slot.planeStrides[1] = slot.stride;
    } else if (pixelFormat_ == V4L2_PIX_FMT_P010) {
        slot.layout = capture::DmaBufLayout::P010;
        slot.stride = std::max(bytesPerLine_, width_ * 2);
    } else if (pixelFormat_ == V4L2_PIX_FMT_RGB24) {
        slot.layout = capture::DmaBufLayout::Rgb888;
        slot.stride = std::max(bytesPerLine_, width_ * 3);
    } else if (pixelFormat_ == V4L2_PIX_FMT_BGR24) {
        slot.layout = capture::DmaBufLayout::Bgr888;
        slot.stride = std::max(bytesPerLine_, width_ * 3);
        slot.flipVertical = true;
    } else if (pixelFormat_ == V4L2_PIX_FMT_YUYV || pixelFormat_ == v4l2_fourcc('Y', 'U', 'Y', '2')) {
        slot.layout = capture::DmaBufLayout::Yuyv422;
        slot.stride = std::max(bytesPerLine_, width_ * 2);
    } else if (pixelFormat_ == V4L2_PIX_FMT_YUV420) {
        slot.layout = capture::DmaBufLayout::I420;
        slot.stride = std::max(bytesPerLine_, width_);
    } else if (pixelFormat_ == V4L2_PIX_FMT_YVU420) {
        slot.layout = capture::DmaBufLayout::Yv12;
        slot.stride = std::max(bytesPerLine_, width_);
    } else {
        return {};
    }

    // The deleter sets a bit in the atomic bitmask so handleReadyRead() can drain all pending
    // requeues in one pass. When the bitmask transitions empty→non-empty we also post a
    // QueuedConnection event so the capture thread wakes up even if no V4L2 frames are arriving
    // (which would otherwise deadlock if all buffers were simultaneously pending requeue).
    // The pool shared_ptr is captured to keep the DmaBufFrame slots alive for as long as any
    // handle is outstanding — safe even if the session is destroyed first.
    auto pending = requeuePending_;
    auto pool = dmaFramePool_;
    const auto bufferIndex = static_cast<int>(buffer.index);
    const QPointer<V4L2CaptureSession> session(this);
    return capture::DmaBufFrameHandle(&slot,
        [pending = std::move(pending), pool = std::move(pool), bufferIndex, session](capture::DmaBufFrame *) {
            // No delete — slot is owned by the pool, not the handle.
            if (session) {
                session->noteRequeueRequested(bufferIndex);
            }
            const auto prev = pending->fetch_or(uint64_t{1} << bufferIndex, std::memory_order_release);
            if (prev == 0 && session) {
                // First bit in this batch — wake the capture thread so it calls drainRequeuePending().
                QMetaObject::invokeMethod(session.data(), [session]() {
                    if (session) {
                        session->drainRequeuePending();
                    }
                }, Qt::QueuedConnection);
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

    const auto bufferIndex =
        frame->captureBufferIndex >= 0 ? frame->captureBufferIndex : frame->bufferIndex;
    const auto bytesUsed = frame->bytesUsed;
    const auto capturedAtNs = frame->capturedAtNs;
    const auto wakeAtNs = frame->wakeAtNs;

    if (!streaming_ || fd_ < 0 || bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) {
        frame.reset();
        return;
    }

    const auto decodeStartNs = capture::monotonicClockNs();
    auto cpuFrame = decodeFrame(buffers_[static_cast<size_t>(bufferIndex)].start, bytesUsed);
    const auto decodeNs = capture::monotonicClockNs() - decodeStartNs;
    frame.reset();

    if (cpuFrame && !cpuFrame->isNull()) {
        recordDecodedFrame(bytesUsed, decodeNs, false);
        emit frameReady(std::move(cpuFrame), capturedAtNs, wakeAtNs);
    }
}

void V4L2CaptureSession::recordDmaFramePresented(const int bytesUsed)
{
    if (bytesUsed <= 0 || !streaming_ || fd_ < 0) {
        return;
    }
    recordDecodedFrame(bytesUsed, 0, true);
}

void V4L2CaptureSession::drainRequeuePending()
{
    // Atomically take all pending bits, then requeue each flagged buffer.
    // Called at the top of handleReadyRead() on the capture thread.
    auto mask = requeuePending_->exchange(0, std::memory_order_acquire);
    while (mask != 0) {
        const int index = __builtin_ctzll(mask); // index of lowest set bit
        mask &= mask - 1;                         // clear lowest set bit
        requeueCaptureBuffer(index);
    }
}

void V4L2CaptureSession::requeueCaptureBuffer(const int bufferIndex)
{
    if (!streaming_ || fd_ < 0 || bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) {
        return;
    }

    const auto traceEnabled = frame_trace::enabled();
    const auto requeueNs = traceEnabled ? capture::monotonicClockNs() : qint64{0};

    v4l2_buffer buffer {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = static_cast<__u32>(bufferIndex);

    if (xioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
        emit logMessage(
            QStringLiteral("V4L2 DMA-BUF requeue failed for buffer %1: %2")
                .arg(bufferIndex)
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    if (traceEnabled) {
        const auto idx = static_cast<std::size_t>(bufferIndex);
        frame_trace::BufferLifetimeSpan span;
        span.seq = buffers_[idx].traceSeq;
        span.event = "v4l2-requeue";
        span.captureBufferIndex = bufferIndex;
        if (buffers_[idx].dqMonotonicNs != 0) {
            span.v4l2HoldMs = static_cast<double>(requeueNs - buffers_[idx].dqMonotonicNs) / 1'000'000.0;
        }
        if (idx < kTraceCaptureBufferSlots) {
            const auto requestedNs = requeueRequestedNs_[idx].load(std::memory_order_relaxed);
            if (requestedNs != 0) {
                span.requeueDelayMs = static_cast<double>(requeueNs - requestedNs) / 1'000'000.0;
            }
        }
        span.outstandingCaptureBuffers =
            outstandingCaptureBuffers_.fetch_sub(1, std::memory_order_relaxed) - 1;
        frame_trace::recordBufferLifetime(span);
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
