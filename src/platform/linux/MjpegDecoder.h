#pragma once

#include "capture/CaptureTypes.h"
#include "platform/linux/MjpegDecoderBackends.h"

#include <QString>

#include <cstddef>
#include <cstdint>

namespace consolation::platform::linux::mjpeg_backend {
class VaapiDecoder;
class V4l2M2mDecoder;
} // namespace consolation::platform::linux::mjpeg_backend

namespace consolation::platform::linux {

// Decodes MJPEG capture buffers to packed ARGB (RGB32). Probes, in order:
//   1. VA-API (Intel/AMD via libva) when built with CONSOLATION_HAVE_LIBVA
//   2. V4L2 memory-to-memory JPEG decoder (many ARM boards, Raspberry Pi 4 and earlier)
//   3. libyuv + libjpeg-turbo (software)
//   4. Qt image loader (last resort)
class MjpegDecoder final {
public:
    MjpegDecoder() = default;
    ~MjpegDecoder();

    MjpegDecoder(const MjpegDecoder &) = delete;
    MjpegDecoder &operator=(const MjpegDecoder &) = delete;

    void configure(int maxWidth, int maxHeight);
    void reset();

    [[nodiscard]] bool decode(
        const uint8_t *data,
        int bytesUsed,
        int dstWidth,
        int dstHeight,
        uint8_t *dstArgb,
        int dstStride);

    [[nodiscard]] bool supportsDmaDecode() const;
    [[nodiscard]] bool dmaSlotsAvailable() const;
    [[nodiscard]] bool decodeDma(const uint8_t *data, int bytesUsed, mjpeg_backend::VaapiDmaFrame &out);
    void releaseDmaSlot(int slotIndex);

    [[nodiscard]] capture::MjpegDecodePath lastPath() const
    {
        return lastPath_;
    }

    [[nodiscard]] bool hardwareDecodeActive() const
    {
        return lastPath_ == capture::MjpegDecodePath::Vaapi || lastPath_ == capture::MjpegDecodePath::V4l2M2m;
    }

    [[nodiscard]] static QString pathLabel(capture::MjpegDecodePath path);

private:
    enum class ActiveBackend {
        None,
        Vaapi,
        V4l2M2m,
    };

    [[nodiscard]] bool decodeSoftware(
        const uint8_t *data,
        int bytesUsed,
        int dstWidth,
        int dstHeight,
        uint8_t *dstArgb,
        int dstStride,
        bool allowQtFallback);

    void disableVaapi();
    void disableV4l2M2m();

    int maxWidth_ = 0;
    int maxHeight_ = 0;
    ActiveBackend activeBackend_ = ActiveBackend::None;
    capture::MjpegDecodePath lastPath_ = capture::MjpegDecodePath::None;

    mjpeg_backend::VaapiDecoder *vaapi_ = nullptr;
    mjpeg_backend::V4l2M2mDecoder *v4l2M2m_ = nullptr;
};

} // namespace consolation::platform::linux
