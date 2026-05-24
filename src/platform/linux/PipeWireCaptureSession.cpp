#include "platform/linux/PipeWireCaptureSession.h"

#include "platform/linux/PipeWireDeviceDiscovery.h"

#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <sys/mman.h>
#include <unistd.h>

namespace consolation::platform::linux {

namespace {

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

void onStateChanged(void *data, const pw_stream_state old, const pw_stream_state state, const char *error)
{
    Q_UNUSED(old);
    static_cast<PipeWireCaptureSession *>(data)->handleStateChanged(state, error);
}

void onParamChanged(void *data, const uint32_t id, const spa_pod *param)
{
    static_cast<PipeWireCaptureSession *>(data)->handleParamChanged(id, param);
}

void onProcess(void *data)
{
    static_cast<PipeWireCaptureSession *>(data)->handleProcess();
}

const pw_stream_events streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = onStateChanged,
    .param_changed = onParamChanged,
    .process = onProcess,
};

} // namespace

PipeWireCaptureSession::PipeWireCaptureSession(QObject *parent)
    : capture::CaptureSession(parent)
    , streamListener_(new spa_hook {})
    , videoInfo_(new spa_video_info_raw {})
{
}

PipeWireCaptureSession::~PipeWireCaptureSession()
{
    stop();
    delete static_cast<spa_hook *>(streamListener_);
    delete static_cast<spa_video_info_raw *>(videoInfo_);
}

bool PipeWireCaptureSession::start(const capture::CaptureDevice &device, const capture::CaptureFormat &format)
{
    Q_UNUSED(format);
    if (!retryingStart_) {
        formatErrorRetries_ = 0;
        useNodeNameTarget_ = false;
        pendingDevice_ = device;
        pendingFormat_ = format;
    } else {
        retryingStart_ = false;
    }

    stop();

    static bool initialized = false;
    if (!initialized) {
        pw_init(nullptr, nullptr);
        initialized = true;
    }

    threadLoop_ = pw_thread_loop_new("consolation-pipewire-capture", nullptr);
    if (threadLoop_ == nullptr) {
        emit failed(QStringLiteral("Could not create PipeWire thread loop."));
        return false;
    }

    pw_thread_loop_lock(static_cast<pw_thread_loop *>(threadLoop_));

    context_ = pw_context_new(pw_thread_loop_get_loop(static_cast<pw_thread_loop *>(threadLoop_)), nullptr, 0);
    if (context_ == nullptr) {
        pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
        emit failed(QStringLiteral("Could not create PipeWire context."));
        stop();
        return false;
    }

    core_ = pw_context_connect(static_cast<pw_context *>(context_), nullptr, 0);
    if (core_ == nullptr) {
        pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
        emit failed(QStringLiteral("Could not connect to PipeWire."));
        stop();
        return false;
    }

    preferredTargetObject_ = useNodeNameTarget_ && !device.nodeName.isEmpty()
        ? device.nodeName
        : PipeWireDeviceDiscovery().resolveTargetObject(device);
    const bool hasTargetObject = !preferredTargetObject_.isEmpty();
    emit logMessage(QStringLiteral("PipeWire target stableId=%1 nodeId=%2 connectTarget=%3 v4l2=%4")
                        .arg(device.stableId)
                        .arg(device.backendNodeId)
                        .arg(hasTargetObject ? preferredTargetObject_ : QStringLiteral("auto"))
                        .arg(device.v4l2DevicePath));
    auto *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE,
        "Video",
        PW_KEY_MEDIA_CATEGORY,
        "Capture",
        PW_KEY_MEDIA_ROLE,
        "Camera",
        nullptr);
    if (hasTargetObject) {
        // PipeWire selects the camera via PW_KEY_TARGET_OBJECT (object serial or node
        // name); pw_stream_connect() must use PW_ID_ANY with AUTOCONNECT.
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, preferredTargetObject_.toUtf8().constData());
    }
    stream_ = pw_stream_new(static_cast<pw_core *>(core_), "Consolation Video Capture", properties);
    if (stream_ == nullptr) {
        pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
        emit failed(QStringLiteral("Could not create PipeWire stream."));
        stop();
        return false;
    }

    pw_stream_add_listener(
        static_cast<pw_stream *>(stream_),
        static_cast<spa_hook *>(streamListener_),
        &streamEvents,
        this);

    // Keep PipeWire format offers minimal. USB capture cards often reset when the
    // PipeWire v4l2 source runs extended VIDIOC_S_FMT negotiation across many formats.
    static constexpr spa_video_format offeredFormat = SPA_VIDEO_FORMAT_YUY2;

    uint8_t formatBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(formatBuffer, sizeof(formatBuffer));
    const spa_rectangle defaultSize = SPA_RECTANGLE(1280, 720);
    const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maxSize = SPA_RECTANGLE(4096, 4096);
    const spa_pod *params[2];
    uint32_t paramCount = 0;
    params[paramCount++] = reinterpret_cast<spa_pod *>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_Id(offeredFormat),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize)));

    spa_video_info_raw withModifier = SPA_VIDEO_INFO_RAW_INIT(
        .format = offeredFormat,
        .flags = SPA_VIDEO_FLAG_MODIFIER,
        .modifier = 0);
    params[paramCount++] = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &withModifier);

    const auto connectFlags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_DONT_RECONNECT |
        PW_STREAM_FLAG_ASYNC |
        PW_STREAM_FLAG_MAP_BUFFERS);

    const auto result = pw_stream_connect(
        static_cast<pw_stream *>(stream_),
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        connectFlags,
        params,
        paramCount);
    if (result < 0) {
        pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
        emit failed(QStringLiteral("Could not connect PipeWire stream: %1").arg(QString::fromLocal8Bit(std::strerror(-result))));
        stop();
        return false;
    }
    emit logMessage(QStringLiteral("PipeWire stream connected; starting thread loop"));

    const auto startResult = pw_thread_loop_start(static_cast<pw_thread_loop *>(threadLoop_));
    pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
    if (startResult < 0) {
        emit failed(QStringLiteral("Could not start PipeWire thread loop."));
        stop();
        return false;
    }

    started_ = true;
    return true;
}

void PipeWireCaptureSession::stop()
{
    started_ = false;
    if (threadLoop_ != nullptr) {
        pw_thread_loop_stop(static_cast<pw_thread_loop *>(threadLoop_));
        pw_thread_loop_lock(static_cast<pw_thread_loop *>(threadLoop_));
    }
    if (stream_ != nullptr) {
        pw_stream_destroy(static_cast<pw_stream *>(stream_));
        stream_ = nullptr;
    }
    if (core_ != nullptr) {
        pw_core_disconnect(static_cast<pw_core *>(core_));
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(static_cast<pw_context *>(context_));
        context_ = nullptr;
    }
    if (threadLoop_ != nullptr) {
        pw_thread_loop_unlock(static_cast<pw_thread_loop *>(threadLoop_));
        pw_thread_loop_destroy(static_cast<pw_thread_loop *>(threadLoop_));
        threadLoop_ = nullptr;
    }
}

void PipeWireCaptureSession::iteratePipeWire()
{
}

void PipeWireCaptureSession::emitCaptureFailed(const QString &message)
{
    QMetaObject::invokeMethod(this, [this, message]() { emit failed(message); }, Qt::QueuedConnection);
}

void PipeWireCaptureSession::scheduleFormatRetry()
{
    if (formatErrorRetries_ >= 3) {
        emitCaptureFailed(QStringLiteral(
            "PipeWire could not negotiate a video format. The capture card may have reset during USB reconnect; try Play again after the device settles."));
        return;
    }

    ++formatErrorRetries_;
    useNodeNameTarget_ = formatErrorRetries_ == 2;
    const auto delayMs = 1500 * formatErrorRetries_;
    emit logMessage(QStringLiteral("PipeWire format negotiation failed; retrying in %1 ms (attempt %2)")
                        .arg(delayMs)
                        .arg(formatErrorRetries_));

    QMetaObject::invokeMethod(
        this,
        [this, delayMs]() {
            QTimer::singleShot(delayMs, this, [this]() {
                retryingStart_ = true;
                static_cast<void>(start(pendingDevice_, pendingFormat_));
            });
        },
        Qt::QueuedConnection);
}

void PipeWireCaptureSession::handleStateChanged(const int state, const char *error)
{
    const auto message = QStringLiteral("state changed to %1 (%2)%3")
                             .arg(state)
                             .arg(QString::fromUtf8(pw_stream_state_as_string(static_cast<pw_stream_state>(state))))
                             .arg(error == nullptr ? QString() : QStringLiteral(" error=%1").arg(QString::fromUtf8(error)));
    QMetaObject::invokeMethod(this, [this, message]() { emit logMessage(message); }, Qt::QueuedConnection);

    if (state == PW_STREAM_STATE_ERROR) {
        const auto errorText = error == nullptr ? QString() : QString::fromUtf8(error);
        if (errorText.contains(QStringLiteral("no more input formats"), Qt::CaseInsensitive)) {
            QMetaObject::invokeMethod(this, [this]() { scheduleFormatRetry(); }, Qt::QueuedConnection);
            return;
        }

        emitCaptureFailed(QStringLiteral("PipeWire stream failed: %1")
                              .arg(errorText.isEmpty() ? QStringLiteral("unknown error") : errorText));
    }
}

void PipeWireCaptureSession::handleParamChanged(const uint32_t id, const void *param)
{
    if (id != SPA_PARAM_Format || param == nullptr) {
        return;
    }

    auto *info = static_cast<spa_video_info_raw *>(videoInfo_);
    spa_zero(*info);
    if (spa_format_video_raw_parse(static_cast<const spa_pod *>(param), info) < 0) {
        QMetaObject::invokeMethod(
            this,
            [this]() { emit failed(QStringLiteral("PipeWire negotiated a video format Consolation could not parse.")); },
            Qt::QueuedConnection);
        return;
    }

    const auto negotiated = QStringLiteral("negotiated format=%1 size=%2x%3 fps=%4/%5")
                                .arg(info->format)
                                .arg(info->size.width)
                                .arg(info->size.height)
                                .arg(info->framerate.num)
                                .arg(info->framerate.denom);
    QMetaObject::invokeMethod(this, [this, negotiated]() { emit logMessage(negotiated); }, Qt::QueuedConnection);

    const uint32_t stride = info->format == SPA_VIDEO_FORMAT_YUY2
        ? info->size.width * 2
        : info->format == SPA_VIDEO_FORMAT_NV12
            ? info->size.width
            : info->size.width * 4;
    const uint32_t size = info->format == SPA_VIDEO_FORMAT_NV12
        ? info->size.width * info->size.height * 3 / 2
        : stride * info->size.height;
    const auto buffers = QStringLiteral("request buffers size=%1 stride=%2").arg(size).arg(stride);
    QMetaObject::invokeMethod(this, [this, buffers]() { emit logMessage(buffers); }, Qt::QueuedConnection);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    params[0] = reinterpret_cast<spa_pod *>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_ParamBuffers,
        SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_blocks,
        SPA_POD_CHOICE_RANGE_Int(0, 1, 16),
        SPA_PARAM_BUFFERS_size,
        SPA_POD_CHOICE_RANGE_Int(size, 1, INT32_MAX),
        SPA_PARAM_BUFFERS_stride,
        SPA_POD_CHOICE_RANGE_Int(stride, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_DmaBuf))));
    const auto result = pw_stream_update_params(static_cast<pw_stream *>(stream_), params, 1);
    const auto updateMessage = QStringLiteral("pw_stream_update_params result=%1").arg(result);
    QMetaObject::invokeMethod(this, [this, updateMessage]() { emit logMessage(updateMessage); }, Qt::QueuedConnection);
}

void PipeWireCaptureSession::handleProcess()
{
    if (stream_ == nullptr || !started_) {
        return;
    }

    pw_buffer *buffer = nullptr;
    while ((buffer = pw_stream_dequeue_buffer(static_cast<pw_stream *>(stream_))) != nullptr) {
        const auto image = imageFromBuffer(buffer->buffer);
        if (!image.isNull()) {
            QMetaObject::invokeMethod(this, [this, image]() { emit frameReady(image); }, Qt::QueuedConnection);
        }
        pw_stream_queue_buffer(static_cast<pw_stream *>(stream_), buffer);
    }
}

QImage PipeWireCaptureSession::imageFromBuffer(void *buffer) const
{
    auto *spaBuffer = static_cast<spa_buffer *>(buffer);
    auto *info = static_cast<spa_video_info_raw *>(videoInfo_);
    if (spaBuffer == nullptr || spaBuffer->n_datas == 0 || info->size.width == 0 || info->size.height == 0) {
        return {};
    }

    const auto &data = spaBuffer->datas[0];
    if (data.chunk == nullptr || (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0) {
        return {};
    }

    void *mapped = nullptr;
    size_t mappedSize = 0;
    const void *sourceData = data.data;
    if (sourceData == nullptr && data.fd >= 0 && data.maxsize > 0 &&
        (data.type == SPA_DATA_MemFd || data.type == SPA_DATA_DmaBuf)) {
        mappedSize = data.mapoffset + data.maxsize;
        mapped = mmap(nullptr, mappedSize, PROT_READ, MAP_PRIVATE, data.fd, 0);
        if (mapped == MAP_FAILED) {
            return {};
        }
        sourceData = static_cast<const uchar *>(mapped) + data.mapoffset;
    }
    if (sourceData == nullptr) {
        return {};
    }

    const auto *bytes = static_cast<const uchar *>(sourceData) + data.chunk->offset;
    const int width = static_cast<int>(info->size.width);
    const int height = static_cast<int>(info->size.height);
    const int fallbackStride = info->format == SPA_VIDEO_FORMAT_YUY2 ? width * 2 : width * 4;
    const int stride = data.chunk->stride > 0 ? data.chunk->stride : fallbackStride;

    if (info->format == SPA_VIDEO_FORMAT_BGRx) {
        QImage image(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            const auto *source = bytes + y * stride;
            auto *target = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                target[x] = qRgb(source[0], source[1], source[2]);
                source += 4;
            }
        }
        if (mapped != nullptr) {
            munmap(mapped, mappedSize);
        }
        return image;
    }

    if (info->format == SPA_VIDEO_FORMAT_RGBx) {
        QImage image(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            const auto *source = bytes + y * stride;
            auto *target = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                target[x] = qRgb(source[2], source[1], source[0]);
                source += 4;
            }
        }
        if (mapped != nullptr) {
            munmap(mapped, mappedSize);
        }
        return image;
    }

    if (info->format == SPA_VIDEO_FORMAT_YUY2) {
        QImage image(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const auto *source = bytes + y * stride;
            for (int x = 0; x < width; x += 2) {
                const int y0 = source[0];
                const int u = source[1];
                const int y1 = source[2];
                const int v = source[3];
                line[x] = yuvToRgb(y0, u, v);
                if (x + 1 < width) {
                    line[x + 1] = yuvToRgb(y1, u, v);
                }
                source += 4;
            }
        }
        if (mapped != nullptr) {
            munmap(mapped, mappedSize);
        }
        return image;
    }

    if (info->format == SPA_VIDEO_FORMAT_NV12) {
        const auto *yPlane = bytes;
        const auto yStride = stride > 0 ? stride : width;
        const auto *uvPlane = bytes + yStride * height;
        const auto uvStride = yStride;

        QImage image(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const auto *yLine = yPlane + y * yStride;
            const auto *uvLine = uvPlane + (y / 2) * uvStride;
            for (int x = 0; x < width; ++x) {
                const int uvIndex = (x / 2) * 2;
                line[x] = yuvToRgb(yLine[x], uvLine[uvIndex], uvLine[uvIndex + 1]);
            }
        }
        if (mapped != nullptr) {
            munmap(mapped, mappedSize);
        }
        return image;
    }

    if (mapped != nullptr) {
        munmap(mapped, mappedSize);
    }
    return {};
}

} // namespace consolation::platform::linux
