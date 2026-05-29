#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace consolation::platform::linux::mjpeg_backend {

struct VaapiDmaFrame {
    int dmaFd = -1;
    std::array<int, 2> planeFds { -1, -1 };
    std::array<int, 2> planeOffsets { 0, 0 };
    std::array<int, 2> planeStrides { 0, 0 };
    std::array<uint32_t, 2> planeFourccs { 0, 0 };
    std::array<uint64_t, 2> planeModifiers { ~uint64_t { 0 }, ~uint64_t { 0 } };
    int width = 0;
    int height = 0;
    int stride = 0;
    int slotIndex = -1;
    // Frame-trace join key, allocated by the decoder when CONSOLATION_FRAME_TRACE is set; 0 otherwise.
    uint64_t seq = 0;
};

class VaapiDecoder final {
public:
    VaapiDecoder() = default;
    ~VaapiDecoder();

    VaapiDecoder(const VaapiDecoder &) = delete;
    VaapiDecoder &operator=(const VaapiDecoder &) = delete;

    [[nodiscard]] bool initialize(int maxWidth, int maxHeight);
    void disable();
    [[nodiscard]] bool dmaExportSupported() const;
    [[nodiscard]] bool dmaSlotsAvailable() const;
    [[nodiscard]] bool decode(
        const uint8_t *data,
        int bytesUsed,
        int dstWidth,
        int dstHeight,
        uint8_t *dstArgb,
        int dstStride);
    [[nodiscard]] bool decodeDma(const uint8_t *data, int bytesUsed, VaapiDmaFrame &out);
    void releaseDmaSlot(int slotIndex);

private:
    struct Impl;
    Impl *impl_ = nullptr;
    bool disabled_ = false;
};

class V4l2M2mDecoder final {
public:
    V4l2M2mDecoder() = default;
    ~V4l2M2mDecoder();

    V4l2M2mDecoder(const V4l2M2mDecoder &) = delete;
    V4l2M2mDecoder &operator=(const V4l2M2mDecoder &) = delete;

    [[nodiscard]] bool initialize(int maxWidth, int maxHeight);
    void disable();
    [[nodiscard]] bool decode(
        const uint8_t *data,
        int bytesUsed,
        int dstWidth,
        int dstHeight,
        uint8_t *dstArgb,
        int dstStride);

private:
    struct Impl;
    Impl *impl_ = nullptr;
    bool disabled_ = false;
};

} // namespace consolation::platform::linux::mjpeg_backend
