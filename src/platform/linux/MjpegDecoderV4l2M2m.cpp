#include "platform/linux/MjpegDecoderBackends.h"

#include <libyuv/convert_argb.h>

#include <QByteArray>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace consolation::platform::linux::mjpeg_backend {

namespace {

int xioctl(const int fd, const unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

bool deviceSupportsMjpegDecode(const int fd)
{
    v4l2_capability capability {};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        return false;
    }

    const auto caps = capability.capabilities;
    const auto deviceCaps = (caps & V4L2_CAP_DEVICE_CAPS) != 0 ? capability.device_caps : caps;
    if ((deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) == 0 && (deviceCaps & V4L2_CAP_VIDEO_M2M) == 0) {
        return false;
    }

    const auto outputType =
        (deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0 ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    const auto captureType =
        (deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0 ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    bool outputMjpeg = false;
    v4l2_fmtdesc format {};
    format.type = outputType;
    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
        if (format.pixelformat == V4L2_PIX_FMT_MJPEG || format.pixelformat == V4L2_PIX_FMT_JPEG) {
            outputMjpeg = true;
            break;
        }
    }

    bool captureNv12 = false;
    format = {};
    format.type = captureType;
    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
        if (format.pixelformat == V4L2_PIX_FMT_NV12) {
            captureNv12 = true;
            break;
        }
    }

    return outputMjpeg && captureNv12;
}

int openMjpegDecoderDevice()
{
    for (int index = 0; index < 64; ++index) {
        const QByteArray path = QByteArrayLiteral("/dev/video") + QByteArray::number(index);
        const auto fd = ::open(path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (deviceSupportsMjpegDecode(fd)) {
            return fd;
        }
        ::close(fd);
    }
    return -1;
}

} // namespace

struct V4l2M2mDecoder::Impl {
    int fd = -1;
    bool multiplanar = false;
    int width = 0;
    int height = 0;
    uint32_t outputType = 0;
    uint32_t captureType = 0;
    std::vector<uint8_t> nv12Scratch;

    struct MappedBuffer {
        void *start = nullptr;
        size_t length = 0;
    };

    MappedBuffer outputBuffer {};
    MappedBuffer captureBuffer {};

    void release()
    {
        if (fd >= 0) {
            if (outputType != 0) {
                xioctl(fd, VIDIOC_STREAMOFF, &outputType);
            }
            if (captureType != 0) {
                xioctl(fd, VIDIOC_STREAMOFF, &captureType);
            }
        }

        if (outputBuffer.start != nullptr && outputBuffer.start != MAP_FAILED) {
            ::munmap(outputBuffer.start, outputBuffer.length);
        }
        if (captureBuffer.start != nullptr && captureBuffer.start != MAP_FAILED) {
            ::munmap(captureBuffer.start, captureBuffer.length);
        }

        outputBuffer = {};
        captureBuffer = {};
        nv12Scratch.clear();

        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    [[nodiscard]] bool setFormat(const int maxWidth, const int maxHeight)
    {
        v4l2_format outputFormat {};
        outputFormat.type = outputType;
        if (multiplanar) {
            outputFormat.fmt.pix_mp.width = static_cast<uint32_t>(maxWidth);
            outputFormat.fmt.pix_mp.height = static_cast<uint32_t>(maxHeight);
            outputFormat.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
            outputFormat.fmt.pix_mp.field = V4L2_FIELD_NONE;
            outputFormat.fmt.pix_mp.num_planes = 1;
        } else {
            outputFormat.fmt.pix.width = static_cast<uint32_t>(maxWidth);
            outputFormat.fmt.pix.height = static_cast<uint32_t>(maxHeight);
            outputFormat.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            outputFormat.fmt.pix.field = V4L2_FIELD_NONE;
        }

        if (xioctl(fd, VIDIOC_S_FMT, &outputFormat) != 0) {
            return false;
        }

        v4l2_format captureFormat {};
        captureFormat.type = captureType;
        if (multiplanar) {
            captureFormat.fmt.pix_mp.width = static_cast<uint32_t>(maxWidth);
            captureFormat.fmt.pix_mp.height = static_cast<uint32_t>(maxHeight);
            captureFormat.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            captureFormat.fmt.pix_mp.field = V4L2_FIELD_NONE;
            captureFormat.fmt.pix_mp.num_planes = 1;
        } else {
            captureFormat.fmt.pix.width = static_cast<uint32_t>(maxWidth);
            captureFormat.fmt.pix.height = static_cast<uint32_t>(maxHeight);
            captureFormat.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
            captureFormat.fmt.pix.field = V4L2_FIELD_NONE;
        }

        if (xioctl(fd, VIDIOC_S_FMT, &captureFormat) != 0) {
            return false;
        }

        if (multiplanar) {
            width = static_cast<int>(captureFormat.fmt.pix_mp.width);
            height = static_cast<int>(captureFormat.fmt.pix_mp.height);
        } else {
            width = static_cast<int>(captureFormat.fmt.pix.width);
            height = static_cast<int>(captureFormat.fmt.pix.height);
        }

        const auto nv12Size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U / 2U;
        nv12Scratch.resize(nv12Size);
        return width > 0 && height > 0;
    }

    [[nodiscard]] bool queueBuffer(const uint32_t type, const int index) const
    {
        if (multiplanar) {
            v4l2_buffer buffer {};
            buffer.type = type;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = static_cast<__u32>(index);
            buffer.length = 1;
            v4l2_plane plane {};
            buffer.m.planes = &plane;
            return xioctl(fd, VIDIOC_QBUF, &buffer) == 0;
        }

        v4l2_buffer buffer {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<__u32>(index);
        return xioctl(fd, VIDIOC_QBUF, &buffer) == 0;
    }

    [[nodiscard]] bool mapQueues()
    {
        auto mapOne = [&](const uint32_t type, MappedBuffer &mapped) -> bool {
            v4l2_requestbuffers request {};
            request.count = 1;
            request.type = type;
            request.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 1) {
                return false;
            }

            v4l2_buffer buffer {};
            buffer.type = type;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = 0;
            v4l2_plane plane {};
            if (multiplanar) {
                buffer.length = 1;
                buffer.m.planes = &plane;
            }

            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
                return false;
            }

            if (multiplanar) {
                mapped.length = plane.length;
                mapped.start = ::mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
            } else {
                mapped.length = buffer.length;
                mapped.start = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
            }

            return mapped.start != MAP_FAILED;
        };

        return mapOne(outputType, outputBuffer) && mapOne(captureType, captureBuffer);
    }

    [[nodiscard]] bool startStreaming()
    {
        return xioctl(fd, VIDIOC_STREAMON, &outputType) == 0 && xioctl(fd, VIDIOC_STREAMON, &captureType) == 0;
    }

    [[nodiscard]] bool decodeFrame(const uint8_t *data, const int bytesUsed)
    {
        if (outputBuffer.start == nullptr || captureBuffer.start == nullptr || data == nullptr || bytesUsed <= 0) {
            return false;
        }

        if (!queueBuffer(captureType, 0)) {
            return false;
        }

        if (static_cast<size_t>(bytesUsed) > outputBuffer.length) {
            return false;
        }

        std::memcpy(outputBuffer.start, data, static_cast<size_t>(bytesUsed));

        if (multiplanar) {
            v4l2_buffer outputQueued {};
            outputQueued.type = outputType;
            outputQueued.memory = V4L2_MEMORY_MMAP;
            outputQueued.index = 0;
            outputQueued.length = 1;
            v4l2_plane outputPlane {};
            outputQueued.m.planes = &outputPlane;
            outputPlane.bytesused = static_cast<__u32>(bytesUsed);
            if (xioctl(fd, VIDIOC_QBUF, &outputQueued) != 0) {
                return false;
            }
        } else {
            v4l2_buffer outputQueued {};
            outputQueued.type = outputType;
            outputQueued.memory = V4L2_MEMORY_MMAP;
            outputQueued.index = 0;
            outputQueued.bytesused = static_cast<__u32>(bytesUsed);
            if (xioctl(fd, VIDIOC_QBUF, &outputQueued) != 0) {
                return false;
            }
        }

        pollfd waiter {};
        waiter.fd = fd;
        waiter.events = POLLIN | POLLOUT;
        if (::poll(&waiter, 1, 250) <= 0) {
            return false;
        }

        if (multiplanar) {
            v4l2_buffer outputDone {};
            outputDone.type = outputType;
            outputDone.memory = V4L2_MEMORY_MMAP;
            outputDone.length = 1;
            v4l2_plane outputPlane {};
            outputDone.m.planes = &outputPlane;
            if (xioctl(fd, VIDIOC_DQBUF, &outputDone) != 0) {
                return false;
            }
        } else {
            v4l2_buffer outputDone {};
            outputDone.type = outputType;
            outputDone.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &outputDone) != 0) {
                return false;
            }
        }

        if (multiplanar) {
            v4l2_buffer captureDone {};
            captureDone.type = captureType;
            captureDone.memory = V4L2_MEMORY_MMAP;
            captureDone.length = 1;
            v4l2_plane capturePlane {};
            captureDone.m.planes = &capturePlane;
            if (xioctl(fd, VIDIOC_DQBUF, &captureDone) != 0) {
                return false;
            }
        } else {
            v4l2_buffer captureDone {};
            captureDone.type = captureType;
            captureDone.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &captureDone) != 0) {
                return false;
            }
        }

        const auto nv12Bytes = std::min(nv12Scratch.size(), captureBuffer.length);
        std::memcpy(nv12Scratch.data(), captureBuffer.start, nv12Bytes);
        return true;
    }
};

V4l2M2mDecoder::~V4l2M2mDecoder()
{
    delete impl_;
}

bool V4l2M2mDecoder::initialize(const int maxWidth, const int maxHeight)
{
    delete impl_;
    impl_ = nullptr;
    disabled_ = false;

    const auto fd = openMjpegDecoderDevice();
    if (fd < 0) {
        return false;
    }

    auto *impl = new Impl();
    impl->fd = fd;

    v4l2_capability capability {};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        delete impl;
        return false;
    }

    const auto caps = capability.capabilities;
    const auto deviceCaps = (caps & V4L2_CAP_DEVICE_CAPS) != 0 ? capability.device_caps : caps;
    impl->multiplanar = (deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    impl->outputType = impl->multiplanar ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    impl->captureType = impl->multiplanar ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (!impl->setFormat(maxWidth, maxHeight) || !impl->mapQueues() || !impl->startStreaming()) {
        impl->release();
        delete impl;
        return false;
    }

    impl_ = impl;
    return true;
}

void V4l2M2mDecoder::disable()
{
    disabled_ = true;
}

bool V4l2M2mDecoder::decode(
    const uint8_t *data,
    const int bytesUsed,
    const int dstWidth,
    const int dstHeight,
    uint8_t *dstArgb,
    const int dstStride)
{
    if (disabled_ || impl_ == nullptr || data == nullptr || bytesUsed <= 0 || dstArgb == nullptr) {
        return false;
    }

    if (!impl_->decodeFrame(data, bytesUsed)) {
        return false;
    }

    const auto *yPlane = impl_->nv12Scratch.data();
    const auto *uvPlane = yPlane + static_cast<size_t>(impl_->width) * static_cast<size_t>(impl_->height);
    const auto result = libyuv::NV12ToARGB(
        yPlane,
        impl_->width,
        uvPlane,
        impl_->width,
        dstArgb,
        dstStride,
        dstWidth,
        dstHeight);
    return result == 0;
}

} // namespace consolation::platform::linux::mjpeg_backend
