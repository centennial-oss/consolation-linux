#include "platform/linux/MjpegDecoder.h"

#include "platform/linux/MjpegDecoderBackends.h"

#include <QImage>

#include <libyuv/convert.h>
#include <libyuv/convert_argb.h>

#include <cstring>

namespace consolation::platform::linux {

namespace {

#if defined(__aarch64__) || defined(__arm__)
constexpr bool kPreferV4l2M2mFirst = true;
#else
constexpr bool kPreferV4l2M2mFirst = false;
#endif

} // namespace

MjpegDecoder::~MjpegDecoder()
{
    reset();
}

void MjpegDecoder::configure(const int maxWidth, const int maxHeight)
{
    if (maxWidth <= 0 || maxHeight <= 0) {
        return;
    }

    if (maxWidth_ == maxWidth && maxHeight_ == maxHeight && activeBackend_ != ActiveBackend::None) {
        return;
    }

    reset();
    maxWidth_ = maxWidth;
    maxHeight_ = maxHeight;

    const auto tryVaapi = [&]() -> bool {
        auto *decoder = new mjpeg_backend::VaapiDecoder();
        if (decoder->initialize(maxWidth_, maxHeight_)) {
            vaapi_ = decoder;
            activeBackend_ = ActiveBackend::Vaapi;
            return true;
        }
        delete decoder;
        return false;
    };

    const auto tryV4l2 = [&]() -> bool {
        auto *decoder = new mjpeg_backend::V4l2M2mDecoder();
        if (decoder->initialize(maxWidth_, maxHeight_)) {
            v4l2M2m_ = decoder;
            activeBackend_ = ActiveBackend::V4l2M2m;
            return true;
        }
        delete decoder;
        return false;
    };

    if (!kPreferV4l2M2mFirst && tryVaapi()) {
        return;
    }
    if (tryV4l2()) {
        return;
    }
    if (kPreferV4l2M2mFirst && tryVaapi()) {
        return;
    }

    activeBackend_ = ActiveBackend::None;
}

void MjpegDecoder::reset()
{
    delete vaapi_;
    vaapi_ = nullptr;
    delete v4l2M2m_;
    v4l2M2m_ = nullptr;
    activeBackend_ = ActiveBackend::None;
    maxWidth_ = 0;
    maxHeight_ = 0;
    lastPath_ = capture::MjpegDecodePath::None;
}

bool MjpegDecoder::decode(
    const uint8_t *data,
    const int bytesUsed,
    const int dstWidth,
    const int dstHeight,
    uint8_t *dstArgb,
    const int dstStride)
{
    if (data == nullptr || bytesUsed <= 0 || dstWidth <= 0 || dstHeight <= 0 || dstArgb == nullptr || dstStride <= 0) {
        return false;
    }

    if (maxWidth_ <= 0 || maxHeight_ <= 0) {
        configure(dstWidth, dstHeight);
    }

    if (activeBackend_ == ActiveBackend::Vaapi && vaapi_ != nullptr) {
        if (!vaapi_->dmaSlotsAvailable()) {
            return decodeSoftware(data, bytesUsed, dstWidth, dstHeight, dstArgb, dstStride, true);
        }
        if (vaapi_->decode(data, bytesUsed, dstWidth, dstHeight, dstArgb, dstStride)) {
            lastPath_ = capture::MjpegDecodePath::Vaapi;
            return true;
        }
        disableVaapi();
    }

    if (activeBackend_ == ActiveBackend::V4l2M2m && v4l2M2m_ != nullptr) {
        if (v4l2M2m_->decode(data, bytesUsed, dstWidth, dstHeight, dstArgb, dstStride)) {
            lastPath_ = capture::MjpegDecodePath::V4l2M2m;
            return true;
        }
        disableV4l2M2m();
    }

    return decodeSoftware(data, bytesUsed, dstWidth, dstHeight, dstArgb, dstStride, true);
}

bool MjpegDecoder::supportsDmaDecode() const
{
    return activeBackend_ == ActiveBackend::Vaapi && vaapi_ != nullptr && vaapi_->dmaExportSupported();
}

bool MjpegDecoder::dmaSlotsAvailable() const
{
    return activeBackend_ == ActiveBackend::Vaapi && vaapi_ != nullptr && vaapi_->dmaSlotsAvailable();
}

bool MjpegDecoder::decodeDma(const uint8_t *data, const int bytesUsed, mjpeg_backend::VaapiDmaFrame &out)
{
    out = {};
    if (data == nullptr || bytesUsed <= 0) {
        return false;
    }

    if (activeBackend_ == ActiveBackend::Vaapi && vaapi_ != nullptr) {
        if (vaapi_->decodeDma(data, bytesUsed, out)) {
            lastPath_ = capture::MjpegDecodePath::Vaapi;
            return true;
        }
    }

    return false;
}

void MjpegDecoder::releaseDmaSlot(const int slotIndex)
{
    if (activeBackend_ == ActiveBackend::Vaapi && vaapi_ != nullptr) {
        vaapi_->releaseDmaSlot(slotIndex);
    }
}

QString MjpegDecoder::pathLabel(const capture::MjpegDecodePath path)
{
    return capture::mjpegDecodePathLabel(path);
}

void MjpegDecoder::disableVaapi()
{
    if (vaapi_ != nullptr) {
        vaapi_->disable();
    }
    if (activeBackend_ == ActiveBackend::Vaapi) {
        activeBackend_ = ActiveBackend::None;
    }
}

void MjpegDecoder::disableV4l2M2m()
{
    if (v4l2M2m_ != nullptr) {
        v4l2M2m_->disable();
    }
    if (activeBackend_ == ActiveBackend::V4l2M2m) {
        activeBackend_ = ActiveBackend::None;
    }
}

bool MjpegDecoder::decodeSoftware(
    const uint8_t *data,
    const int bytesUsed,
    const int dstWidth,
    const int dstHeight,
    uint8_t *dstArgb,
    const int dstStride,
    const bool allowQtFallback)
{
#ifdef HAVE_JPEG
    int jpegWidth = 0;
    int jpegHeight = 0;
    if (libyuv::MJPGSize(data, static_cast<size_t>(bytesUsed), &jpegWidth, &jpegHeight) == 0 && jpegWidth > 0 &&
        jpegHeight > 0) {
        const auto result = libyuv::MJPGToARGB(
            data,
            static_cast<size_t>(bytesUsed),
            dstArgb,
            dstStride,
            jpegWidth,
            jpegHeight,
            dstWidth,
            dstHeight);
        if (result == 0) {
            lastPath_ = capture::MjpegDecodePath::Libyuv;
            return true;
        }
    }
#else
    Q_UNUSED(data);
    Q_UNUSED(bytesUsed);
    Q_UNUSED(dstWidth);
    Q_UNUSED(dstHeight);
    Q_UNUSED(dstArgb);
    Q_UNUSED(dstStride);
#endif

    if (!allowQtFallback) {
        return false;
    }

    const QImage decoded = QImage::fromData(data, bytesUsed).convertToFormat(QImage::Format_RGB32);
    if (decoded.isNull() || decoded.width() != dstWidth || decoded.height() != dstHeight ||
        decoded.bytesPerLine() != dstStride) {
        return false;
    }

    std::memcpy(dstArgb, decoded.constBits(), static_cast<size_t>(decoded.sizeInBytes()));
    lastPath_ = capture::MjpegDecodePath::Qt;
    return true;
}

} // namespace consolation::platform::linux
